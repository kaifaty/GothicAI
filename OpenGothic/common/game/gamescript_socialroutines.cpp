#include "gamescript.h"

#include "world/objects/npc.h"

#include <algorithm>
#include <array>
#include <bit>

namespace {
std::string numberedLine(std::string_view prefix, int number) {
  return std::string(prefix) + char('0'+number/10) + char('0'+number%10);
  }
}

bool GameScript::runSocialRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  const auto name = std::string_view(symbol.name());
  const bool loop = name.ends_with("_LOOP");
  const bool end  = name.ends_with("_END");
  const bool init = !loop && !end;
  const auto state = name.substr(0,name.size()-(loop ? 5 : end ? 4 : 0));
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());

  auto read = [&](std::string_view field) {
    return vm.find_symbol_by_name(field)->get_int();
    };
  auto write = [&](std::string_view field, int value) {
    vm.find_symbol_by_name(field)->set_int(value);
    };
  auto id = [&](std::string_view field) {
    return int(vm.find_symbol_by_name(field)->index());
    };
  auto instance = [&](std::string_view field) {
    return vm.find_symbol_by_name(field)->get_instance();
    };
  auto matches = [&](std::string_view field) {
    return hlp_getinstanceid(instance(field))==int(npc.instanceSymbol());
    };
  auto draw = [&](uint32_t range, std::string_view field) {
    const auto value = int(rollRandom(range,&npc,true));
    write(field,value);
    return value;
    };
  auto reset = [&]() {
    if(hlp_isvalidnpc(npc_getlookattarget(self)))
      ai_stoplookat(self);
    ai_removeweapon(self);
    };
  auto goIfFar = [&](bool align) {
    if(npc_getdisttowp(self,self->wp)>500) {
      ai_gotowp(self,self->wp);
      if(align)
        ai_aligntowp(self);
      }
    };
  auto perception = [&](PercType type, std::string_view handler) {
    npc.setPerceptionEnable(type,size_t(id(handler)));
    };
  auto& position = self->aivar[19];
  const auto seconds = int(npc.stateTime()/1000);

  if(state=="ZS_SMALLTALK") {
    if(init) {
      setPerceptionPreset(true);
      reset();
      ai_setwalkmode(self,1);
      goIfFar(false);
      }
    else if(loop) {
      if(!npc.isOnFreePoint("SMALLTALK")) {
        if(wld_isfpavailable(self,"SMALLTALK")) {
          ai_gotofp(self,"SMALLTALK");
          ai_standup(self);
          ai_aligntofp(self);
          }
        }
      else if(seconds>=8) {
        const bool detected = wld_detectnpc(self,-1,id("ZS_SMALLTALK"),-1);
        const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
        if(detected && npc_getdisttonpc(self,other)<500) {
          npc_setstatetime(other,4);
          ai_turntonpc(self,other);
          const auto roll = draw(120,"B_SAY_SMALLTALK.RANDOM");
          // Hlp_Random(1) always returns zero and still consumes a draw.
          draw(1,"B_SAY_SMALLTALK.CHOICE");
          const auto guild = npc.trueGuild();
          auto line = 1+roll/5;
          if(roll>=105) {
            if(guild==GIL_KDF || guild==GIL_PAL || guild==GIL_NOV)
              line = roll<110 ? 28 : roll<115 ? 29 : 30;
            else if(guild==GIL_PIR || guild==GIL_DJG || guild==GIL_SLD || guild==GIL_BDT)
              line = roll<110 ? 25 : roll<115 ? 26 : 27;
            else
              line = roll<110 ? 22 : roll<115 ? 23 : 24;
            }
          ai_outputsvm(self,self,numberedLine("$SMALLTALK",line));
          }
        npc.setStateTime(0);
        }
      }
    }
  else if(state=="ZS_DANCE") {
    if(init) {
      setPerceptionPreset(true);
      reset();
      ai_setwalkmode(self,1);
      goIfFar(true);
      }
    else if(loop) {
      const auto dance = draw(9,"ZS_DANCE_LOOP.DANCESTYLE");
      ai_playani(self,numberedLine("T_DANCE_",dance+1));
      ai_gotowp(self,self->wp);
      ai_aligntowp(self);
      }
    else {
      ai_standup(self);
      }
    }
  else if(state=="ZS_ANNOUNCE_HEROLD" || state=="ZS_PREACH_VATRAS") {
    const bool herald = state=="ZS_ANNOUNCE_HEROLD";
    if(init) {
      setPerceptionPreset(true);
      npc_percdisable(self,PERC_ASSESSQUIETSOUND);
      reset();
      ai_setwalkmode(self,1);
      ai_gotowp(self,self->wp);
      ai_aligntowp(self);
      if(herald)
        position = 1;
      }
    else if(loop && herald) {
      const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(instance("HERO"));
      if(seconds>=70 && (hero==nullptr || hero->aivar[4]==0)) {
        ai_playani(self,"T_HGUARD_2_STAND");
        position = 1;
        if(ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),BS_SIT)) {
          ai_standup(self);
          ai_turntonpc(self,hero);
          }
        ai_removeweapon(self);
        const auto announcement = id("FAKESCROLL");
        createinvitem(self,announcement);
        ai_useitemtostate(self,announcement,1);
        auto output = [&](int n) { ai_output(self,self,numberedLine("DIA_Herold_Announce_04_",n)); };
        output(0);
        const auto chapter = storyChapter;
        if(chapter<=2) {
          static constexpr std::array starts = {1,3,5,7,11,13};
          const auto choice = draw(5,"B_ANNOUNCE_HEROLD.RANDY");
          for(auto n=starts[choice]; n<starts[choice+1]; ++n)
            output(n);
          }
        else if(chapter==3) {
          if(read("MIS_RESCUEBENNET")==2)
            output(15);
          else {
            output(13);
            output(14);
            }
          }
        else if(chapter==4) {
          output(16);
          output(17);
          output(18);
          }
        else {
          const auto choice = draw(2,"B_ANNOUNCE_HEROLD.RANDY");
          output(19+2*choice);
          output(20+2*choice);
          }
        ai_useitemtostate(self,announcement,-1);
        npc.setStateTime(0);
        }
      if(position==1) {
        ai_playani(self,"T_STAND_2_HGUARD");
        position = 0;
        }
      }
    else if(loop && seconds>=13) {
      if(position>20)
        position = 0;
      write("B_PREACH_VATRAS.SATZ",position);
      if(position>=0)
        ai_output(self,self,numberedLine("DIA_Vatras_PREACH_05_",position));
      ++position;
      npc.setStateTime(0);
      }
    }
  else if(state=="ZS_CONCERT") {
    static constexpr std::array musicians = {
      std::pair{"PIR_1394_ADDON_INEXTREMO_YELLOWPFEIFFER","ITMI_IEDUDELGELB"},
      std::pair{"PIR_1390_ADDON_INEXTREMO_DRPYMONTE",      "ITMI_IEHARFE"},
      std::pair{"PIR_1391_ADDON_INEXTREMO_THEFLAIL",      "ITMI_IEDRUM"},
      std::pair{"PIR_1392_ADDON_INEXTREMO_THOMASTHEFORGER","ITMI_IECELLO"},
      std::pair{"PIR_1393_ADDON_INEXTREMO_UNICORN",       "ITMI_IELAUTE"},
      std::pair{"PIR_1395_ADDON_INEXTREMO_LUTTER",        "ITMI_IEDRUMSCHEIT"},
      std::pair{"PIR_1396_ADDON_INEXTREMO_FLEX",          "ITMI_IEDUDELBLAU"},
      };
    if(init) {
      setPerceptionPreset(true);
      reset();
      ai_setwalkmode(self,1);
      goIfFar(false);
      position = 1;
      }
    else if(end || position==1) {
      for(const auto& [musician,item] : musicians) {
        // The final musician has an enter command in the original only.
        if(matches(musician) && (!end || std::string_view(item)!="ITMI_IEDUDELBLAU")) {
          ai_useitemtostate(self,id(item),end ? -1 : 1);
          break;
          }
        }
      if(loop)
        position = 0;
      }
    }
  else if(state=="ZS_GHOST") {
    if(init) {
      const auto spirit = hlp_getnpc(id("NONE_ADDON_111_QUARHODRON"));
      vm.find_symbol_by_name("ZS_GHOST.QUARHO")->set_instance(spirit);
      if(hlp_getinstanceid(spirit)==int(npc.instanceSymbol())) {
        if(read("GHOST_SCKNOWSHOW2GETINADANOSTEMPEL")==0)
          perception(PERC_ASSESSTALK,"B_ASSESSTALK");
        }
      else {
        perception(PERC_ASSESSTALK,"B_ASSESSTALK");
        perception(PERC_ASSESSDAMAGE,"B_ASSESSDAMAGE");
        }
      reset();
      npc_setperctime(self,0.1f);
      ai_standup(self);
      ai_setwalkmode(self,1);
      ai_gotowp(self,self->wp);
      ai_aligntowp(self);
      }
    else if(loop && seconds>=5) {
      if(npc_getdisttonpc(self,std::dynamic_pointer_cast<daedalus::INpc>(instance("HERO")))>500) {
        ai_aligntowp(self);
        npc.setStateTime(0);
        }
      // This helper implements the quest's one-shot removal and sound effects.
      vm.call_function<void>("B_REMOVEGHOST");
      }
    }
  else if(state=="ZS_GHOSTWUSEL") {
    constexpr std::string_view roam = "FP_ROAM";
    if(init) {
      perception(PERC_ASSESSTALK,"B_ASSESSTALK");
      perception(PERC_ASSESSDAMAGE,"B_ASSESSDAMAGE");
      reset();
      ai_standup(self);
      ai_setwalkmode(self,1);
      if(npc_getnearestwp(self)!=self->wp)
        ai_gotowp(self,self->wp);
      ai_gotofp(self,roam);
      }
    else if(loop && seconds>=3) {
      if(npc.isOnFreePoint(roam)) {
        if(wld_isnextfpavailable(self,roam)) {
          npc_clearaiqueue(self);
          ai_gotonextfp(self,roam);
          }
        }
      else if(!ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),BS_RUN) &&
              !ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),BS_WALK)) {
        ai_gotofp(self,roam);
        }
      npc.setStateTime(0);
      }
    }
  else if(state=="ZS_CIRCLE") {
    if(init) {
      setPerceptionPreset(true);
      reset();
      npc_setperctime(self,0.3f);
      ai_standup(self);
      ai_setwalkmode(self,1);
      ai_gotowp(self,self->wp);
      ai_aligntowp(self);
      }
    else if(loop) {
      auto effect = [&](std::string_view name, const std::shared_ptr<daedalus::DaedalusInstance>& target) {
        wld_playeffect(name,target,target,0,0,0,0);
        };
      const auto guild = self->guild;
      if(guild==GIL_DMT) {
        if(seconds>draw(3000,"ZS_CIRCLE_LOOP.RANDY")) {
          effect("FX_EarthQuake",self);
          npc.setStateTime(0);
          effect("spellFX_INCOVATION_RED",self);
          ai_playani(self,"T_PRACTICEMAGIC5");
          }
        }
      else if(guild==GIL_KDW) {
        const auto threshold = draw(2500,"ZS_CIRCLE_LOOP.RANDYKDW");
        const auto lightStar = draw(2,"ZS_CIRCLE_LOOP.RANDYKDW_LIGHTSTAR");
        if(seconds>threshold) {
          ai_aligntowp(self);
          npc.setStateTime(0);
          effect("FX_EarthQuake",self);
          ai_playani(self,"T_PRACTICEMAGIC5");
          const auto anvil = instance("ITMI_AMBOSSEFFEKT_ADDON");
          effect("spellFX_RingRitual2",anvil);
          effect(lightStar==0 ? "spellFX_RingRitual1" : "spellFX_LIGHTSTAR_RingRitual",anvil);
          }
        }
      else if(read("MIS_RITUALINNOSEYEREPAIR")!=2 && seconds>draw(1000,"ZS_CIRCLE_LOOP.RANDY")) {
        npc.setStateTime(0);
        effect("FX_EarthQuake",self);
        ai_playani(self,"T_PRACTICEMAGIC5");
        if(matches("XARDAS"))
          effect("spellFX_INCOVATION_VIOLET",self);
        else if(matches("PYROKAR"))
          effect("spellFX_INCOVATION_RED",self);
        else
          effect("spellFX_INCOVATION_BLUE",self);
        }
      }
    }
  else if(state=="ZS_STAND_DEMENTOR" || state=="ZS_STAND_RANGERMEETING") {
    const bool ranger = state=="ZS_STAND_RANGERMEETING";
    const bool lares = ranger && matches("LARES");
    if(init) {
      if(ranger) {
        setPerceptionPreset(false);
        reset();
        self->senses = 7;
        self->senses_range = 500;
        npc_setperctime(self,1.f);
        perception(PERC_ASSESSPLAYER,"B_ASSESSGUIDEPLAYER");
        perception(PERC_ASSESSENEMY,"B_ASSESSENEMY");
        const bool greg = matches("GREG_NW");
        if(greg || lares || matches("EROL")) {
          perception(PERC_ASSESSTALK,"B_ASSESSTALK");
          perception(PERC_MOVEMOB,"B_MOVEMOB");
          perception(PERC_ASSESSFIGHTSOUND,"B_ASSESSGUIDEFIGHTSOUND");
          }
        if(greg || lares) {
          ai_setwalkmode(self,0);
          if(npc_getdisttowp(self,self->wp)>500)
            ai_gotowp(self,self->wp);
          ai_aligntowp(self);
          ai_playani(self,"T_STAND_2_HGUARD");
          }
        else {
          ai_setwalkmode(self,1);
          goIfFar(false);
          position = 1;
          }
        }
      else {
        self->senses = 7;
        self->senses_range = 1500;
        npc_setperctime(self,npc_knowsinfo(self,1) ? 0.3f : 1.f);
        perception(PERC_ASSESSPLAYER,"B_ASSESSPLAYER");
        perception(PERC_ASSESSENEMY,"B_ASSESSENEMY");
        perception(PERC_ASSESSMAGIC,"B_ASSESSMAGIC");
        perception(PERC_ASSESSDAMAGE,"B_ASSESSDAMAGE");
        if(!matches("DMT_1299_OBERDEMENTOR_DI")) {
          perception(PERC_ASSESSFIGHTSOUND,"B_ASSESSFIGHTSOUND");
          perception(PERC_ASSESSWARN,"B_ASSESSWARN");
          perception(PERC_ASSESSTALK,"B_ASSESSTALK");
          perception(PERC_MOVEMOB,"B_MOVEMOB");
          }
        reset();
        ai_setwalkmode(self,1);
        goIfFar(false);
        position = 1;
        }
      }
    else if(loop && lares) {
      if(seconds>5) {
        if(draw(2,"ZS_STAND_RANGERMEETING_LOOP.RANDOM")==0)
          ai_playani(self,"T_HGUARD_LOOKAROUND");
        npc.setStateTime(0);
        }
      }
    else if(loop) {
      if(npc.isOnFreePoint("STAND")) {
        ai_aligntofp(self);
        if(position==2)
          position = 1;
        }
      else if(wld_isfpavailable(self,"STAND")) {
        ai_gotofp(self,"STAND");
        ai_standup(self);
        ai_aligntofp(self);
        position = 2;
        }
      else {
        ai_aligntowp(self);
        if(position==2)
          position = 1;
        }
      if(position==1) {
        ai_standup(self);
        ai_playani(self,"T_STAND_2_LGUARD");
        position = 0;
        }
      if(ranger && position==0 && seconds>5) {
        static constexpr std::array gestures = {"T_LGUARD_SCRATCH","T_LGUARD_STRETCH","T_LGUARD_CHANGELEG"};
        const auto choice = draw(7,"ZS_STAND_RANGERMEETING_LOOP.RANDOM");
        if(size_t(choice)<gestures.size())
          ai_playani(self,gestures[choice]);
        npc.setStateTime(0);
        }
      }
    else if(!lares) {
      ai_playani(self,"T_LGUARD_2_STAND");
      }
    }
  else {
    return false;
    }
  result = LOOP_CONTINUE;
  return true;
  }

bool GameScript::runTalk(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  if(ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;
  constexpr size_t startedTalk = 3; // AIV_NPCSTARTEDTALK in the verified program
  constexpr size_t invincible = 4; // AIV_INVINCIBLE
  constexpr size_t talkedToPlayer = 5; // AIV_TALKEDTOPLAYER
  constexpr size_t partyMember = 15; // AIV_PARTYMEMBER
  constexpr size_t toughGuyNewsOverride = 25; // AIV_TOUGHGUYNEWSOVERRIDE
  const auto self = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto seated = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    return ActivityDefinitions::bodyStateContains(npc_getbodystate(actor),BS_SIT);
    };
  const auto predicate = [&](std::string_view name) { return vm.call_function<int32_t>(name,self)!=0; };
  auto& talkFix = *findSymbol("ZSTALKBUGFIX");

  const std::string_view name = symbol.name();
  if(name=="ZS_TALK") {
    const auto player = other();
    result = 0;
    if(player && player->aivar[invincible]==1)
      return true;
    self->aivar[invincible] = 1;
    if(player)
      player->aivar[invincible] = 1;

    if(self->guild<int32_t(GIL_SEPERATOR_HUM)) {
      if(seated(self)) {
        const auto target = npc_getlookattarget(self);
        findSymbol("ZS_TALK.TARGET")->set_instance(target);
        if(!hlp_isvalidnpc(target))
          ai_lookatnpc(self,other());
        } else {
        lookAtNpc(self,other());
        }
      ai_removeweapon(self);
      }
    if(!seated(self))
      vm.call_function<void>("B_TURNTONPC",self,other());
    if(!seated(other())) {
      vm.call_function<void>("B_TURNTONPC",other(),self);
      if(npc_getdisttonpc(other(),self)<80)
        ai_dodge(other());
      }

    if(self->guild<int32_t(GIL_SEPERATOR_HUM)) {
      const auto attitude = npc_getattitude(self,other());
      if(attitude==ATT_HOSTILE || attitude==ATT_ANGRY) {
        const bool bandit = self->guild==GIL_BDT;
        const bool fakeBandit = vm.call_function<int32_t>("C_PLAYERISFAKEBANDIT",self,other())!=0;
        if(!bandit || !fakeBandit)
          mdl_startfaceani(self,"S_ANGRY",1.f,-1.f);
        }
      if(self->type==NpcType::G2_TAL_AMBIENT || self->type==NpcType::G2_BL_AMBIENT ||
         self->type==NpcType::G2_OC_AMBIENT || self->type==NpcType::G2_AMBIENT) {
        vm.call_function<void>("B_ASSIGNAMBIENTINFOS",self);
        if(predicate("C_NPCBELONGSTOCITY"))
          vm.call_function<void>("B_ASSIGNCITYGUIDE",self);
        }
      static constexpr std::string_view noPartyPotion[] = {"PARDOS_NW","PARDOS","BIFF_DI","BIFF_NW","BIFF"};
      const auto selfId = hlp_getinstanceid(self);
      const bool excluded = std::ranges::any_of(noPartyPotion,[&](std::string_view instance) {
        return hlp_getinstanceid(findSymbol(instance)->get_instance())==selfId;
        });
      if(!excluded && self->aivar[partyMember]==1)
        vm.call_function<void>("B_ADDON_GIVEPOTION",self);
      const bool canOfferNews = self->aivar[toughGuyNewsOverride]==0;
      const bool toughGuy = predicate("C_NPCISTOUGHGUY");
      if(canOfferNews && toughGuy)
        vm.call_function<void>("B_ASSIGNTOUGHGUYNEWS",self);
      if(predicate("C_NPCHASAMBIENTNEWS"))
        vm.call_function<void>("B_ASSIGNAMBIENTNEWS",self);
      }
    if(self->guild==GIL_DRAGON)
      ai_playani(self,"T_STAND_2_TALK");
    ai_processinfos(self);
    talkFix.set_int(0);
    return true;
    }

  if(name=="ZS_TALK_LOOP") {
    const bool hasWaited = talkFix.get_int()==1;
    const bool finished = infomanager_hasfinished();
    if(!hasWaited || !finished) {
      talkFix.set_int(1);
      result = LOOP_CONTINUE;
      return true;
      }
    self->aivar[invincible] = 0;
    if(const auto player=other())
      player->aivar[invincible] = 0;
    self->aivar[startedTalk] = 0;
    self->aivar[talkedToPlayer] = 1;
    if(self->guild<int32_t(GIL_SEPERATOR_HUM)) {
      stopLookAtNpc(self);
      mdl_startfaceani(self,"S_NEUTRAL",1.f,-1.f);
      }
    if(self->guild==GIL_DRAGON)
      ai_playani(self,"T_TALK_2_STAND");
    result = LOOP_END;
    return true;
    }

  if(name=="ZS_TALK_END") {
    npc_setrefusetalk(other(),20);
    const bool unfriendly = npc_getattitude(self,other())!=ATT_FRIENDLY;
    const bool publicRoom = wld_getplayerportalguild()==int32_t(GIL_PUBLIC);
    const bool bothered = predicate("C_NPCISBOTHEREDBYPLAYERROOMGUILD");
    if((unfriendly && publicRoom) || bothered)
      ai_startstate(self,int(findFunction("ZS_OBSERVEPLAYER").ptr),0,"");
    result = 0;
    return true;
    }
  return false;
  }

void GameScript::endProductionDialog() {
  constexpr size_t invincible = 4; // AIV_INVINCIBLE
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  ai_stopprocessinfos(self);
  wld_stopeffect("DEMENTOR_FX");
  if(self)
    self->aivar[invincible] = 0;
  findSymbol("PLAYER_MOBSI_PRODUCTION")->set_int(findSymbol("MOBSI_NONE")->get_int());
  }

bool GameScript::canPickpocket(int32_t dexterity, int32_t gold) {
  constexpr size_t pickedPocket = 6; // AIV_PLAYERHASPICKEDMYPOCKET
  findSymbol("C_BEKLAUEN.THEFTGOLD")->set_int(gold);
  findSymbol("C_BEKLAUEN.THEFTDEX")->set_int(dexterity);
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const bool possessed = findSymbol("NPCOBSESSEDBYDMT")->get_int()!=0;
  const auto difference = findSymbol("THEFTDIFF")->get_int();
  const auto minimum = std::bit_cast<int32_t>(uint32_t(dexterity)-uint32_t(difference));
  const auto playerDexterity = other ? other->attribute[ATR_DEXTERITY] : 0;
  const bool robbed = self && self->aivar[pickedPocket]!=0;
  const bool trained = npc_gettalentskill(other,TALENT_PICKPOCKET)==1;
  if(possessed || playerDexterity<minimum || robbed || !trained)
    return false;
  if(npc_isinstate(self,int(ZS_Talk))) {
    findSymbol("THEFTDEXGLOB")->set_int(dexterity<=20 ? 10 : dexterity);
    findSymbol("THEFTGOLDGLOB")->set_int(gold);
    }
  return true;
  }

void GameScript::attemptPickpocket() {
  constexpr size_t pickedPocket = 6; // AIV_PLAYERHASPICKEDMYPOCKET
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto playerDexterity = other ? other->attribute[ATR_DEXTERITY] : 0;
  if(playerDexterity>=findSymbol("THEFTDEXGLOB")->get_int()) {
    giveInventoryItems(self,other,int32_t(findSymbolIndex("ITMI_GOLD")),findSymbol("THEFTGOLDGLOB")->get_int());
    if(self)
      self->aivar[pickedPocket] = 1;
    givePickpocketXp();
    snd_play("Geldbeutel");
    return;
    }
  decrementPickpocketVictimCount();
  ai_stopprocessinfos(self);
  if(self)
    startNpcAttack(self,other,findSymbol("AR_THEFT")->get_int(),1);
  }

void GameScript::givePickpocketXp() {
  auto& victims = *findSymbol("VICTIMCOUNT");
  auto& nextLevel = *findSymbol("VICTIMLEVEL");
  auto& level = *findSymbol("THIEFLEVEL");
  const auto count = std::bit_cast<int32_t>(uint32_t(victims.get_int())+1u);
  victims.set_int(count);
  if(nextLevel.get_int()==0)
    nextLevel.set_int(2);
  if(count>=nextLevel.get_int()) {
    const auto next = std::bit_cast<int32_t>(uint32_t(level.get_int())+1u);
    level.set_int(next);
    nextLevel.set_int(std::bit_cast<int32_t>(uint32_t(count)+uint32_t(next)));
    }
  const auto amount = std::bit_cast<int32_t>(10u*uint32_t(level.get_int())+uint32_t(findSymbol("THIEFXP")->get_int()));
  vm.push_int(amount);
  vm.unsafe_call_bl(findSymbol("B_GIVEPLAYERXP"));
  }

void GameScript::decrementPickpocketVictimCount() {
  auto& victims = *findSymbol("VICTIMCOUNT");
  if(victims.get_int()>findSymbol("THIEFLEVEL")->get_int())
    victims.set_int(victims.get_int()-1);
  }

void GameScript::loadNativeTalkReaction() {
  if(!nativeRoutineProgram)
    return;
  vm.override_function("B_ASSESSTALK",std::function<void()>{[this] { runAssessTalk(); }},[this] {
    return canRunNativeTalkReaction();
    });
  }

bool GameScript::canRunNativeTalkReaction() {
  if(!nativeRoutineProgram || !activityEvents || floorDefer.active() ||
     ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;
  const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const auto other = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance());
  const auto* source = findNpc(static_cast<NpcRecord*>(other.get()));
  if(!findNpc(static_cast<NpcRecord*>(self.get())) || !source || !source->isPlayer() ||
     other->symbol_index()!=findSymbolIndex("PC_HERO"))
    return false;

  // An inspector's debug conversation displays room data through the original script.
  const auto isDebugAlias = [&](std::string_view name) {
    auto* instance = findSymbol(name);
    return instance->is_instance_of<daedalus::INpc>() &&
           hlp_getinstanceid(instance->get_instance())==hlp_getinstanceid(other);
    };
  return !isDebugAlias("PC_LEVELINSPEKTOR") && !isDebugAlias("PC_ROCKEFELLER");
  }

bool GameScript::invokeNativeTalkPerception(const daedalus::DaedalusSymbol& symbol, NpcActivityProbe::Phase& phase) {
  if(symbol.name()!="B_ASSESSTALK" || !canRunNativeTalkReaction())
    return false;
  phase.fromRecord();
  vm.unsafe_call_bl(&symbol);
  return true;
  }

bool GameScript::refusesTalk(const std::shared_ptr<daedalus::INpc>& self, const std::shared_ptr<daedalus::INpc>& other) {
  constexpr size_t guardPassageStatus = 12; // AIV_GUARDPASSAGE_STATUS in the verified program
  constexpr size_t talkedToPlayer = 5; // AIV_TALKEDTOPLAYER
  findSymbol("C_REFUSETALK.OTH")->set_instance(other);
  findSymbol("C_REFUSETALK.SLF")->set_instance(self);

  const bool noPass = !self || self->aivar[guardPassageStatus]==0;
  const bool guard = isGateGuard(self);
  const bool refused = npc_refusetalk(self);
  if(noPass && guard && refused)
    return true;
  const bool farFromPost = npc_getdisttowp(self,self ? std::string_view(self->wp) : std::string_view{})>500;
  const bool firstTalk = !self || self->aivar[talkedToPlayer]==0;
  if(farFromPost && firstTalk && hlp_getinstanceid(self)==hlp_getinstanceid(findSymbol("LOTHAR")->get_instance()))
    return true;

  const auto listener = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
  const bool mortal = (!listener || int32_t(listener->flags)!=findSymbol("NPC_FLAG_IMMORTAL")->get_int());
  const bool fakeGuild = hasFakeGuild(self,other);
  return mortal && fakeGuild;
  }

void GameScript::runAssessTalk() {
  constexpr size_t startedTalk = 3; // AIV_NPCSTARTEDTALK in the verified program
  const auto self = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance()); };
  const auto other = [&] { return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_other()->get_instance()); };
  const auto guild = [&] { const auto actor=self(); return actor ? actor->guild : int32_t(GIL_NONE); };
  const auto started = [&] { const auto actor=self(); return actor ? actor->aivar[startedTalk] : 0; };
  const auto attack = [&](std::string_view reason) {
    startNpcAttack(self(),other(),findSymbol(reason)->get_int(),0);
    };

  findSymbol("B_ASSESSTALK.PCL")->set_instance(hlp_getnpc(int(findSymbolIndex("PC_LEVELINSPEKTOR"))));
  findSymbol("B_ASSESSTALK.PCR")->set_instance(hlp_getnpc(int(findSymbolIndex("PC_ROCKEFELLER"))));
  vm.unsafe_set_gi(self());

  if(guild()>int32_t(GIL_SEPERATOR_HUM) && !npc_checkinfo(self(),1) && !npc_checkinfo(self(),0))
    return;
  if(guild()<int32_t(GIL_SEPERATOR_HUM)) {
    vm.unsafe_call_bl(findSymbol("B_ASSESSENEMY"));
    if(vm.pop_int()!=0)
      return;
    const bool wantsAttack = wantsToAttackMurderer(self(),other());
    const bool murder = playerCrime(self())==findSymbol("CRIME_MURDER")->get_int();
    if(wantsAttack && murder) {
      attack("AR_HUMANMURDEREDHUMAN");
      return;
      }
    const bool notBandit = guild()!=int32_t(GIL_BDT);
    const bool fakeBandit = isPlayerFakeBandit(self(),other());
    if(notBandit && fakeBandit) {
      attack("AR_GUILDENEMY");
      return;
      }
    if(refusesTalk(self(),other())) {
      if(hasFakeGuild(self(),other())) {
        npc_clearaiqueue(self());
        ai_startstate(self(),int(findFunction("ZS_COMMENTFAKEGUILD").ptr),1,"");
        } else {
        sayToNpc(self(),other(),"$NOTNOW");
        }
      return;
      }
    }

  if(started()==0) {
    const bool running = npcBodyStateContains(self(),BS_RUN);
    const bool walking = npcBodyStateContains(self(),BS_WALK);
    if(running || walking) {
      sayToNpc(other(),self(),"$SC_HEYWAITASECOND");
      } else if(!npc_canseenpc(self(),other())) {
      const auto roll = hlp_random(100);
      findSymbol("B_ASSESSTALK.RND")->set_int(roll);
      static constexpr std::pair<int32_t,std::string_view> greetings[] = {
        {25,"$SC_HEYTURNAROUND"}, {50,"$SC_HEYTURNAROUND02"}, {75,"$SC_HEYTURNAROUND03"}, {99,"$SC_HEYTURNAROUND04"},
        };
      const auto line = std::ranges::find_if(greetings,[&](const auto& entry) { return roll<=entry.first; });
      if(line!=std::end(greetings))
        sayToNpc(other(),self(),line->second);
      }
    }
  npc_clearaiqueue(self());
  clearPerceptions(self());

  const bool seated = npcBodyStateContains(self(),BS_SIT);
  const bool quick = started()==1;
  const auto stand = [&](const std::shared_ptr<daedalus::INpc>& actor) {
    if(quick)
      ai_standupquick(actor);
    else
      ai_standup(actor);
    };
  if(!seated)
    stand(self());
  stand(other());
  const bool needsTurn = seated && !npc_canseenpc(self(),other());
  if(needsTurn && npc_isinstate(self(),int(findFunction("ZS_OBSERVEPLAYER").ptr)))
    ai_standup(self());
  ai_startstate(self(),int(ZS_Talk),needsTurn ? 1 : 0,"");
  }
