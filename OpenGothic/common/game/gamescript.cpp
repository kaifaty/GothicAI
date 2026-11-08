#include "gamescript.h"

#include <Tempest/Log>
#include <Tempest/SoundEffect>

#include <cctype>
#include <optional>
#include <sstream>

#include "game/definitions/spelldefinitions.h"
#include "game/serialize.h"
#include "game/herodeed.h"
#include "game/healing.h"
#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/triggers/abstracttrigger.h"
#include "graphics/visualfx.h"
#include "utils/fileutil.h"
#include "commandline.h"
#include "gothic.h"
#include "loadprobe.h"
#include "navdemos.h"
#include "testbench.h"

using namespace Tempest;

template <typename T>
struct ScopeVar final {
  ScopeVar(daedalus::DaedalusSymbol& sym, const std::shared_ptr<T>& h) : prev(sym.get_instance()), sym(sym) {
    sym.set_instance(h);
    }

  ScopeVar(const ScopeVar&)=delete;
  ~ScopeVar(){
    sym.set_instance(prev);
    }

  std::shared_ptr<daedalus::DaedalusInstance> prev;
  daedalus::DaedalusSymbol&                   sym;
  };

struct ScopeCtx final {
  ScopeCtx(GameScript& script, NpcProcessPolicy pp) : script(script) {
    prev = script.aiProcessPolicy;
    script.aiProcessPolicy = pp;
    }

  ~ScopeCtx() {
    script.aiProcessPolicy = prev;
    }

  GameScript&      script;
  NpcProcessPolicy prev = NpcProcessPolicy::AiNormal;
  };


bool GameScript::GlobalOutput::output(Npc& npc, std::string_view text) {
  return owner.aiOutput(npc,text,false);
  }

bool GameScript::GlobalOutput::outputSvm(Npc &npc, std::string_view text, std::string_view svm, Npc* target) {
  return owner.aiOutputSvm(npc,text,svm,target,false);
  }

bool GameScript::GlobalOutput::outputOv(Npc &npc, std::string_view text, std::string_view svm, Npc* target) {
  return owner.aiOutputSvm(npc,text,svm,target,true);
  }

bool GameScript::GlobalOutput::printScr(Npc& npc, int time, std::string_view msg, int x, int y, std::string_view font) {
  auto& f = Resources::font(font, Resources::FontType::Normal, 1.0);
  Gothic::inst().onPrintScreen(msg,x,y,time,f);
  return true;
  }

bool GameScript::GlobalOutput::isFinished() {
  return true;
  }

GameScript::PerDist::PerDist() {
  for(auto& i:range)
    i = -1;
  }

int GameScript::PerDist::at(PercType perc, int r) const {
  if(perc>=PERC_Count)
    return r;
  auto rr = range[perc];
  if(rr>0)
    return rr;
  return r;
  }


GameScript::GameScript(GameSession &owner)
    :owner(owner), vm(createVm(Gothic::inst())) {
  if (vm.global_self() == nullptr || vm.global_other() == nullptr || vm.global_item() == nullptr ||
      vm.global_victim() == nullptr || vm.global_hero() == nullptr)
    throw std::runtime_error("Cannot find script symbol SELF, OTHER, ITEM, VICTIM, or HERO! Cannot proceed!");

  vmLang = Gothic::inst().settingsGetI("GAME", "language");
  vm.register_exception_handler(daedalus::lenient_vm_exception_handler);
  Gothic::inst().setupCommonScriptClasses(vm);
  Gothic::inst().setupVmCommonApi(vm);
  aiDefaultPipe.reset(new GlobalOutput(*this));
  initCommon();
  initSettings();
  Gothic::inst().onSettingsChanged.bind(this,&GameScript::initSettings);
  }

GameScript::~GameScript() {
  Gothic::inst().onSettingsChanged.ubind(this,&GameScript::initSettings);
  }

void GameScript::initCommon() {
  bindExternal("hlp_random",                     &GameScript::hlp_random);
  bindExternal("hlp_isvalidnpc",                 &GameScript::hlp_isvalidnpc);
  bindExternal("hlp_isvaliditem",                &GameScript::hlp_isvaliditem);
  bindExternal("hlp_isitem",                     &GameScript::hlp_isitem);
  bindExternal("hlp_getnpc",                     &GameScript::hlp_getnpc);
  bindExternal("hlp_getinstanceid",              &GameScript::hlp_getinstanceid);

  bindExternal("wld_insertnpc",                  &GameScript::wld_insertnpc);
  bindExternal("wld_removenpc",                  &GameScript::wld_removenpc);
  bindExternal("wld_insertitem",                 &GameScript::wld_insertitem);
  bindExternal("wld_settime",                    &GameScript::wld_settime);
  bindExternal("wld_getday",                     &GameScript::wld_getday);
  bindExternal("wld_playeffect",                 &GameScript::wld_playeffect);
  bindExternal("wld_stopeffect",                 &GameScript::wld_stopeffect);
  bindExternal("wld_getplayerportalguild",       &GameScript::wld_getplayerportalguild);
  bindExternal("wld_getformerplayerportalguild", &GameScript::wld_getformerplayerportalguild);
  bindExternal("wld_setguildattitude",           &GameScript::wld_setguildattitude);
  bindExternal("wld_getguildattitude",           &GameScript::wld_getguildattitude);
  bindExternal("wld_exchangeguildattitudes",     &GameScript::wld_exchangeguildattitudes);
  bindExternal("wld_istime",                     &GameScript::wld_istime);
  bindExternal("wld_isfpavailable",              &GameScript::wld_isfpavailable);
  bindExternal("wld_isnextfpavailable",          &GameScript::wld_isnextfpavailable);
  bindExternal("wld_ismobavailable",             &GameScript::wld_ismobavailable);
  bindExternal("wld_setmobroutine",              &GameScript::wld_setmobroutine);
  bindExternal("wld_getmobstate",                &GameScript::wld_getmobstate);
  bindExternal("wld_assignroomtoguild",          &GameScript::wld_assignroomtoguild);
  bindExternal("wld_detectnpc",                  &GameScript::wld_detectnpc);
  bindExternal("wld_detectnpcex",                &GameScript::wld_detectnpcex);
  bindExternal("wld_detectitem",                 &GameScript::wld_detectitem);
  bindExternal("wld_spawnnpcrange",              &GameScript::wld_spawnnpcrange);
  bindExternal("wld_sendtrigger",                &GameScript::wld_sendtrigger);
  bindExternal("wld_senduntrigger",              &GameScript::wld_senduntrigger);
  bindExternal("wld_israining",                  &GameScript::wld_israining);

  bindExternal("mdl_setvisual",                  &GameScript::mdl_setvisual);
  bindExternal("mdl_setvisualbody",              &GameScript::mdl_setvisualbody);
  bindExternal("mdl_setmodelfatness",            &GameScript::mdl_setmodelfatness);
  bindExternal("mdl_applyoverlaymds",            &GameScript::mdl_applyoverlaymds);
  bindExternal("mdl_applyoverlaymdstimed",       &GameScript::mdl_applyoverlaymdstimed);
  bindExternal("mdl_removeoverlaymds",           &GameScript::mdl_removeoverlaymds);
  bindExternal("mdl_setmodelscale",              &GameScript::mdl_setmodelscale);
  bindExternal("mdl_startfaceani",               &GameScript::mdl_startfaceani);
  bindExternal("mdl_applyrandomani",             &GameScript::mdl_applyrandomani);
  bindExternal("mdl_applyrandomanifreq",         &GameScript::mdl_applyrandomanifreq);
  bindExternal("mdl_applyrandomfaceani",         &GameScript::mdl_applyrandomfaceani);

  bindExternal("npc_settofightmode",             &GameScript::npc_settofightmode);
  bindExternal("npc_settofistmode",              &GameScript::npc_settofistmode);
  bindExternal("npc_isinstate",                  &GameScript::npc_isinstate);
  bindExternal("npc_isinroutine",                &GameScript::npc_isinroutine);
  bindExternal("npc_wasinstate",                 &GameScript::npc_wasinstate);
  bindExternal("npc_getdisttowp",                &GameScript::npc_getdisttowp);
  bindExternal("npc_exchangeroutine",            &GameScript::npc_exchangeroutine);
  bindExternal("npc_isdead",                     &GameScript::npc_isdead);
  bindExternal("npc_knowsinfo",                  &GameScript::npc_knowsinfo);
  bindExternal("npc_settalentskill",             &GameScript::npc_settalentskill);
  bindExternal("npc_gettalentskill",             &GameScript::npc_gettalentskill);
  bindExternal("npc_settalentvalue",             &GameScript::npc_settalentvalue);
  bindExternal("npc_gettalentvalue",             &GameScript::npc_gettalentvalue);
  bindExternal("npc_setrefusetalk",              &GameScript::npc_setrefusetalk);
  bindExternal("npc_refusetalk",                 &GameScript::npc_refusetalk);
  bindExternal("npc_hasitems",                   &GameScript::npc_hasitems);
  bindExternal("npc_hasspell",                   &GameScript::npc_hasspell);
  bindExternal("npc_getinvitem",                 &GameScript::npc_getinvitem);
  bindExternal("npc_getinvitembyslot",           &GameScript::npc_getinvitembyslot);
  bindExternal("npc_removeinvitem",              &GameScript::npc_removeinvitem);
  bindExternal("npc_removeinvitems",             &GameScript::npc_removeinvitems);
  bindExternal("npc_getbodystate",               &GameScript::npc_getbodystate);
  bindExternal("npc_getlookattarget",            &GameScript::npc_getlookattarget);
  bindExternal("npc_getdisttonpc",               &GameScript::npc_getdisttonpc);
  bindExternal("npc_hasequippedarmor",           &GameScript::npc_hasequippedarmor);
  bindExternal("npc_setperctime",                &GameScript::npc_setperctime);
  bindExternal("npc_percenable",                 &GameScript::npc_percenable);
  bindExternal("npc_percdisable",                &GameScript::npc_percdisable);
  bindExternal("npc_getnearestwp",               &GameScript::npc_getnearestwp);
  bindExternal("npc_getnextwp",                  &GameScript::npc_getnextwp);
  bindExternal("npc_clearaiqueue",               &GameScript::npc_clearaiqueue);
  bindExternal("npc_isplayer",                   &GameScript::npc_isplayer);
  bindExternal("npc_getstatetime",               &GameScript::npc_getstatetime);
  bindExternal("npc_setstatetime",               &GameScript::npc_setstatetime);
  bindExternal("npc_changeattribute",            &GameScript::npc_changeattribute);
  bindExternal("npc_isonfp",                     &GameScript::npc_isonfp);
  bindExternal("npc_getheighttonpc",             &GameScript::npc_getheighttonpc);
  bindExternal("npc_getequippedmeleeweapon",     &GameScript::npc_getequippedmeleeweapon);
  bindExternal("npc_getequippedrangedweapon",    &GameScript::npc_getequippedrangedweapon);
  bindExternal("npc_getequippedarmor",           &GameScript::npc_getequippedarmor);
  bindExternal("npc_canseenpc",                  &GameScript::npc_canseenpc);
  bindExternal("npc_canseenpcfreelos",           &GameScript::npc_canseenpcfreelos);
  bindExternal("npc_canseeitem",                 &GameScript::npc_canseeitem);
  bindExternal("npc_hasequippedweapon",          &GameScript::npc_hasequippedweapon);
  bindExternal("npc_hasequippedmeleeweapon",     &GameScript::npc_hasequippedmeleeweapon);
  bindExternal("npc_hasequippedrangedweapon",    &GameScript::npc_hasequippedrangedweapon);
  bindExternal("npc_getactivespell",             &GameScript::npc_getactivespell);
  bindExternal("npc_getactivespellisscroll",     &GameScript::npc_getactivespellisscroll);
  bindExternal("npc_getactivespellcat",          &GameScript::npc_getactivespellcat);
  bindExternal("npc_setactivespellinfo",         &GameScript::npc_setactivespellinfo);
  bindExternal("npc_getactivespelllevel",        &GameScript::npc_getactivespelllevel);
  bindExternal("npc_isinfightmode",              &GameScript::npc_isinfightmode);
  bindExternal("npc_settarget",                  &GameScript::npc_settarget);
  bindExternal("npc_gettarget",                  &GameScript::npc_gettarget);
  bindExternal("npc_getnexttarget",              &GameScript::npc_getnexttarget);
  bindExternal("npc_sendpassiveperc",            &GameScript::npc_sendpassiveperc);
  bindExternal("npc_sendsingleperc",             &GameScript::npc_sendsingleperc);
  bindExternal("npc_checkinfo",                  &GameScript::npc_checkinfo);
  bindExternal("npc_getportalguild",             &GameScript::npc_getportalguild);
  bindExternal("npc_isinplayersroom",            &GameScript::npc_isinplayersroom);
  bindExternal("npc_getreadiedweapon",           &GameScript::npc_getreadiedweapon);
  bindExternal("npc_hasreadiedweapon",           &GameScript::npc_hasreadiedweapon);
  bindExternal("npc_hasreadiedmeleeweapon",      &GameScript::npc_hasreadiedmeleeweapon);
  bindExternal("npc_hasreadiedrangedweapon",     &GameScript::npc_hasreadiedrangedweapon);
  bindExternal("npc_hasrangedweaponwithammo",    &GameScript::npc_hasrangedweaponwithammo);
  bindExternal("npc_isdrawingspell",             &GameScript::npc_isdrawingspell);
  bindExternal("npc_isdrawingweapon",            &GameScript::npc_isdrawingweapon);
  bindExternal("npc_perceiveall",                &GameScript::npc_perceiveall);
  bindExternal("npc_stopani",                    &GameScript::npc_stopani);
  bindExternal("npc_settrueguild",               &GameScript::npc_settrueguild);
  bindExternal("npc_gettrueguild",               &GameScript::npc_gettrueguild);
  bindExternal("npc_clearinventory",             &GameScript::npc_clearinventory);
  bindExternal("npc_getattitude",                &GameScript::npc_getattitude);
  bindExternal("npc_getpermattitude",            &GameScript::npc_getpermattitude);
  bindExternal("npc_setattitude",                &GameScript::npc_setattitude);
  bindExternal("npc_settempattitude",            &GameScript::npc_settempattitude);
  bindExternal("npc_hasbodyflag",                &GameScript::npc_hasbodyflag);
  bindExternal("npc_getlasthitspellid",          &GameScript::npc_getlasthitspellid);
  bindExternal("npc_getlasthitspellcat",         &GameScript::npc_getlasthitspellcat);
  bindExternal("npc_playani",                    &GameScript::npc_playani);
  bindExternal("npc_isdetectedmobownedbynpc",    &GameScript::npc_isdetectedmobownedbynpc);
  bindExternal("npc_getdetectedmob",             &GameScript::npc_getdetectedmob);
  bindExternal("npc_isdetectedmobownedbyguild",  &GameScript::npc_isdetectedmobownedbyguild);
  bindExternal("npc_ownedbynpc",                 &GameScript::npc_ownedbynpc);
  bindExternal("npc_canseesource",               &GameScript::npc_canseesource);
  bindExternal("npc_isincutscene",               &GameScript::npc_isincutscene);
  bindExternal("npc_getdisttoitem",              &GameScript::npc_getdisttoitem);
  bindExternal("npc_getheighttoitem",            &GameScript::npc_getheighttoitem);
  bindExternal("npc_getdisttoplayer",            &GameScript::npc_getdisttoplayer);

  bindExternal("ai_output",                      &GameScript::ai_output);
  bindExternal("ai_stopprocessinfos",            &GameScript::ai_stopprocessinfos);
  bindExternal("ai_processinfos",                &GameScript::ai_processinfos);
  bindExternal("ai_standup",                     &GameScript::ai_standup);
  bindExternal("ai_standupquick",                &GameScript::ai_standupquick);
  bindExternal("ai_continueroutine",             &GameScript::ai_continueroutine);
  bindExternal("ai_stoplookat",                  &GameScript::ai_stoplookat);
  bindExternal("ai_lookat",                      &GameScript::ai_lookat);
  bindExternal("ai_lookatnpc",                   &GameScript::ai_lookatnpc);
  bindExternal("ai_removeweapon",                &GameScript::ai_removeweapon);
  bindExternal("ai_unreadyspell",                &GameScript::ai_unreadyspell);
  bindExternal("ai_turnaway",                    &GameScript::ai_turnaway);
  bindExternal("ai_turntonpc",                   &GameScript::ai_turntonpc);
  bindExternal("ai_whirlaround",                 &GameScript::ai_whirlaround);
  bindExternal("ai_outputsvm",                   &GameScript::ai_outputsvm);
  bindExternal("ai_outputsvm_overlay",           &GameScript::ai_outputsvm_overlay);
  bindExternal("ai_startstate",                  &GameScript::ai_startstate);
  bindExternal("ai_playani",                     &GameScript::ai_playani);
  bindExternal("ai_setwalkmode",                 &GameScript::ai_setwalkmode);
  bindExternal("ai_wait",                        &GameScript::ai_wait);
  bindExternal("ai_waitms",                      &GameScript::ai_waitms);
  bindExternal("ai_aligntowp",                   &GameScript::ai_aligntowp);
  bindExternal("ai_gotowp",                      &GameScript::ai_gotowp);
  bindExternal("ai_gotofp",                      &GameScript::ai_gotofp);
  bindExternal("ai_playanibs",                   &GameScript::ai_playanibs);
  bindExternal("ai_equiparmor",                  &GameScript::ai_equiparmor);
  bindExternal("ai_equipbestarmor",              &GameScript::ai_equipbestarmor);
  bindExternal("ai_equipbestmeleeweapon",        &GameScript::ai_equipbestmeleeweapon);
  bindExternal("ai_equipbestrangedweapon",       &GameScript::ai_equipbestrangedweapon);
  bindExternal("ai_usemob",                      &GameScript::ai_usemob);
  bindExternal("ai_teleport",                    &GameScript::ai_teleport);
  bindExternal("ai_stoppointat",                 &GameScript::ai_stoppointat);
  bindExternal("ai_drawweapon",                  &GameScript::ai_drawweapon);
  bindExternal("ai_readymeleeweapon",            &GameScript::ai_readymeleeweapon);
  bindExternal("ai_readyrangedweapon",           &GameScript::ai_readyrangedweapon);
  bindExternal("ai_readyspell",                  &GameScript::ai_readyspell);
  bindExternal("ai_attack",                      &GameScript::ai_attack);
  bindExternal("ai_flee",                        &GameScript::ai_flee);
  bindExternal("ai_dodge",                       &GameScript::ai_dodge);
  bindExternal("ai_unequipweapons",              &GameScript::ai_unequipweapons);
  bindExternal("ai_unequiparmor",                &GameScript::ai_unequiparmor);
  bindExternal("ai_gotonpc",                     &GameScript::ai_gotonpc);
  bindExternal("ai_gotonextfp",                  &GameScript::ai_gotonextfp);
  bindExternal("ai_aligntofp",                   &GameScript::ai_aligntofp);
  bindExternal("ai_useitem",                     &GameScript::ai_useitem);
  bindExternal("ai_useitemtostate",              &GameScript::ai_useitemtostate);
  bindExternal("ai_setnpcstostate",              &GameScript::ai_setnpcstostate);
  bindExternal("ai_finishingmove",               &GameScript::ai_finishingmove);
  bindExternal("ai_takeitem",                    &GameScript::ai_takeitem);
  bindExternal("ai_gotoitem",                    &GameScript::ai_gotoitem);
  bindExternal("ai_pointat",                     &GameScript::ai_pointat);
  bindExternal("ai_pointatnpc",                  &GameScript::ai_pointatnpc);

  bindExternal("mob_hasitems",                   &GameScript::mob_hasitems);
  bindExternal("ai_printscreen",                 &GameScript::ai_printscreen);

  bindExternal("ta_min",                         &GameScript::ta_min);

  bindExternal("log_createtopic",                &GameScript::log_createtopic);
  bindExternal("log_settopicstatus",             &GameScript::log_settopicstatus);
  bindExternal("log_addentry",                   &GameScript::log_addentry);

  bindExternal("equipitem",                      &GameScript::equipitem);
  bindExternal("createinvitem",                  &GameScript::createinvitem);
  bindExternal("createinvitems",                 &GameScript::createinvitems);

  bindExternal("perc_setrange",                  &GameScript::perc_setrange);

  bindExternal("info_addchoice",                 &GameScript::info_addchoice);
  bindExternal("info_clearchoices",              &GameScript::info_clearchoices);
  bindExternal("infomanager_hasfinished",        &GameScript::infomanager_hasfinished);

  bindExternal("snd_play",                       &GameScript::snd_play);
  bindExternal("snd_play3d",                     &GameScript::snd_play3d);

  bindExternal("game_initgerman",                &GameScript::game_initgerman);
  bindExternal("game_initenglish",               &GameScript::game_initenglish);

  bindExternal("exitsession",                    &GameScript::exitsession);

  // vm.validateExternals();

  spells               = std::make_unique<SpellDefinitions>(vm);
  svm                  = std::make_unique<SvmDefinitions>(vm);

  cFocusNorm           = findFocus("Focus_Normal");
  cFocusMelee          = findFocus("Focus_Melee");
  cFocusRange          = findFocus("Focus_Ranged");
  cFocusMage           = findFocus("Focus_Magic");

  ZS_Dead              = aiState(findSymbolIndex("ZS_Dead")).funcIni;
  ZS_Unconscious       = aiState(findSymbolIndex("ZS_Unconscious")).funcIni;
  ZS_Talk              = aiState(findSymbolIndex("ZS_Talk")).funcIni;
  ZS_Attack            = aiState(findSymbolIndex("ZS_Attack")).funcIni;
  ZS_MM_Attack         = aiState(findSymbolIndex("ZS_MM_Attack")).funcIni;

  spellFxInstanceNames = vm.find_symbol_by_name("spellFxInstanceNames");
  spellFxAniLetters    = vm.find_symbol_by_name("spellFxAniLetters");

  if(spellFxInstanceNames==nullptr || spellFxAniLetters==nullptr) {
    throw std::runtime_error("spellFxInstanceNames and/or spellFxAniLetters not found");
    }

  auto* currency = vm.find_symbol_by_name("TRADE_CURRENCY_INSTANCE");
  itMi_Gold      = currency!=nullptr ? vm.find_symbol_by_name(currency->get_string()) : nullptr;
  if(itMi_Gold!=nullptr){ // FIXME
    auto item = vm.init_instance<daedalus::IItem>(itMi_Gold);
    goldTxt = item->name;
    }
  auto* tradeMul = vm.find_symbol_by_name("TRADE_VALUE_MULTIPLIER");
  tradeValMult   = tradeMul != nullptr ? tradeMul->get_float() : 1.0f;

  auto* vtime     = vm.find_symbol_by_name("VIEW_TIME_PER_CHAR");
  viewTimePerChar = vtime != nullptr ? vtime->get_float() : 550.f;
  if(viewTimePerChar<=0.f)
    viewTimePerChar = 550.f;

  ItKE_lockpick     = vm.find_symbol_by_name("ItKE_lockpick");
  B_RefreshAtInsert = vm.find_symbol_by_name("B_RefreshAtInsert");

  if(auto v = vm.find_symbol_by_name("RANGED_CHANCE_MINDIST"))
    rangedChanceMin = v->get_float();
  if(auto v = vm.find_symbol_by_name("RANGED_CHANCE_MAXDIST"))
    rangedChanceMax = v->get_float();

  auto* gilMax = vm.find_symbol_by_name("GIL_MAX");
  gilCount = gilMax!=nullptr ? size_t(gilMax->get_int()) : 0;

  auto* tblSz = vm.find_symbol_by_name("TAB_ANZAHL");
  gilTblSize = tblSz!=nullptr ? size_t(std::sqrt(tblSz->get_int())) : 0;
  gilAttitudes.resize(gilCount*gilCount,ATT_HOSTILE);
  wld_exchangeguildattitudes("GIL_ATTITUDES");

  auto id = vm.find_symbol_by_name("Gil_Values");
  if(id!=nullptr){
    cGuildVal = vm.init_instance<daedalus::IGuildValues>(id);
    for(size_t i=0;i<Guild::GIL_PUBLIC;++i){
      cGuildVal->water_depth_knee   [i]=cGuildVal->water_depth_knee   [Guild::GIL_HUMAN];
      cGuildVal->water_depth_chest  [i]=cGuildVal->water_depth_chest  [Guild::GIL_HUMAN];
      cGuildVal->jumpup_height      [i]=cGuildVal->jumpup_height      [Guild::GIL_HUMAN];
      cGuildVal->swim_time          [i]=cGuildVal->swim_time          [Guild::GIL_HUMAN];
      cGuildVal->dive_time          [i]=cGuildVal->dive_time          [Guild::GIL_HUMAN];
      cGuildVal->step_height        [i]=cGuildVal->step_height        [Guild::GIL_HUMAN];
      cGuildVal->jumplow_height     [i]=cGuildVal->jumplow_height     [Guild::GIL_HUMAN];
      cGuildVal->jumpmid_height     [i]=cGuildVal->jumpmid_height     [Guild::GIL_HUMAN];
      cGuildVal->slide_angle        [i]=cGuildVal->slide_angle        [Guild::GIL_HUMAN];
      cGuildVal->slide_angle2       [i]=cGuildVal->slide_angle2       [Guild::GIL_HUMAN];
      cGuildVal->disable_autoroll   [i]=cGuildVal->disable_autoroll   [Guild::GIL_HUMAN];
      cGuildVal->surface_align      [i]=cGuildVal->surface_align      [Guild::GIL_HUMAN];
      cGuildVal->climb_heading_angle[i]=cGuildVal->climb_heading_angle[Guild::GIL_HUMAN];
      cGuildVal->climb_horiz_angle  [i]=cGuildVal->climb_horiz_angle  [Guild::GIL_HUMAN];
      cGuildVal->climb_ground_angle [i]=cGuildVal->climb_ground_angle [Guild::GIL_HUMAN];
      cGuildVal->fight_range_base   [i]=cGuildVal->fight_range_base   [Guild::GIL_HUMAN];
      cGuildVal->fight_range_fist   [i]=cGuildVal->fight_range_fist   [Guild::GIL_HUMAN];
      cGuildVal->fight_range_g      [i]=cGuildVal->fight_range_g      [Guild::GIL_HUMAN];
      cGuildVal->fight_range_1hs    [i]=cGuildVal->fight_range_1hs    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_1ha    [i]=cGuildVal->fight_range_1ha    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_2hs    [i]=cGuildVal->fight_range_2hs    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_2ha    [i]=cGuildVal->fight_range_2ha    [Guild::GIL_HUMAN];
      cGuildVal->falldown_height    [i]=cGuildVal->falldown_height    [Guild::GIL_HUMAN];
      cGuildVal->falldown_damage    [i]=cGuildVal->falldown_damage    [Guild::GIL_HUMAN];
      cGuildVal->blood_disabled     [i]=cGuildVal->blood_disabled     [Guild::GIL_HUMAN];
      cGuildVal->blood_max_distance [i]=cGuildVal->blood_max_distance [Guild::GIL_HUMAN];
      cGuildVal->blood_amount       [i]=cGuildVal->blood_amount       [Guild::GIL_HUMAN];
      cGuildVal->blood_flow         [i]=cGuildVal->blood_flow         [Guild::GIL_HUMAN];
      cGuildVal->blood_emitter      [i]=cGuildVal->blood_emitter      [Guild::GIL_HUMAN];
      cGuildVal->blood_texture      [i]=cGuildVal->blood_texture      [Guild::GIL_HUMAN];
      cGuildVal->turn_speed         [i]=cGuildVal->turn_speed         [Guild::GIL_HUMAN];
      }
    }

  if(vm.find_symbol_by_name("Ikarus_Version")!=nullptr || vm.find_symbol_by_name("LeGo_Version")!=nullptr)
    Log::e("Ikarus/LeGo scripts are not supported by this build");
  }

void GameScript::initSettings() {
  auto lang = Gothic::inst().settingsGetI("GAME", "language");
  if(vmLang!=lang) {
    vmLang = lang;
    //vm     = createVm(Gothic::inst());
    initDialogs();
    }
  }

void GameScript::initDialogs() {
  loadDialogOU();

  dialogsInfo.clear();
  vm.enumerate_instances_by_class_name("C_INFO", [this](daedalus::DaedalusSymbol& sym){
    dialogsInfo.push_back(vm.init_instance<daedalus::IInfo>(&sym));
    });
  }

void GameScript::loadDialogOU() {
  // Based on https://github.com/auronen/Gothic-2-localization mod,
  // at least in G2 .BIN should have a priority
  const std::string prefix = std::string(Gothic::inst().defaultOutputUnits());
  const std::array<std::string,2> names = {prefix + ".BIN", prefix + ".DAT"};

  for(auto& OU:names) {
    if(Resources::loadBaseOutputUnits(OU,vmLang,dialogs))
      return;
    }
  Log::e("none of Zen-files for OU could be loaded");
  }

void GameScript::initializeInstanceNpc(const std::shared_ptr<daedalus::INpc>& npc, size_t instance) {
  LoadProbe::Startup::Detail::Scope detail("npc_init",[&]{
    const auto* symbol=vm.find_symbol_by_index(uint32_t(instance));
    return symbol!=nullptr ? symbol->name() : std::to_string(instance);
    });
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Script);
  auto sym = vm.find_symbol_by_index(uint32_t(instance));

  if(sym == nullptr) {
    Tempest::Log::e("Cannot initialize NPC ", instance, ": Symbol not found.");
    return;
    }

  // an npc of ours runs the code of the script's npc it is made like, and that code names its own instance
  // (self in an instance's body is the instance's symbol): while it runs that instance is ours, as a script's
  // instance is its own while its code runs. Else the body, clothes and things would go to the script's npc
  std::optional<ScopeVar<daedalus::INpc>> mine;
  if(auto* like = own.likeOf(sym->index()))
    mine.emplace(*like,npc);

  vm.init_instance(npc, sym);

  if(npc->daily_routine!=0) {
    ScopeVar self(*vm.global_self(), npc);
    auto* daily_routine = vm.find_symbol_by_index(uint32_t(npc->daily_routine));

    if(daily_routine != nullptr) {
      LoadProbe::Startup::Detail::Scope routine("daily_routine",[&]{return daily_routine->name();});
      vm.call_function(daily_routine);
      }
    }
  }

void GameScript::initializeInstanceItem(const std::shared_ptr<daedalus::IItem>& item, size_t instance) {
  LoadProbe::Vobs::Scope probe("script_item");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::Script,false,"placed-items");
  auto sym = vm.find_symbol_by_index(uint32_t(instance));

  if(sym == nullptr) {
    Tempest::Log::e("Cannot initialize item ", instance, ": Symbol not found.");
    return;
    }

  vm.init_instance(item, sym);
  }

void GameScript::saveQuests(Serialize &fout) {
  quests.save(fout);
  fout.write(uint32_t(dlgKnownInfos.size()));
  for(auto& i:dlgKnownInfos)
    fout.write(uint32_t(i.first),uint32_t(i.second));

  fout.write(gilAttitudes);
  }

void GameScript::loadQuests(Serialize& fin) {
  quests.load(fin);
  uint32_t sz=0;
  fin.read(sz);
  for(size_t i=0;i<sz;++i){
    uint32_t f=0,s=0;
    fin.read(f,s);
    dlgKnownInfos.insert(std::make_pair(f,s));
    }

  fin.read(gilAttitudes);
  }

void GameScript::saveVar(Serialize &fout) {
  auto& dat = vm.symbols();
  fout.write(uint32_t(dat.size()));
  for(unsigned i = 0; i < dat.size(); ++i){
    auto* sym = vm.find_symbol_by_index(i); // never returns nullptr
    saveSym(fout,*sym);
    }
  }

void GameScript::loadVar(Serialize &fin) {
  std::string name;
  uint32_t sz=0;
  fin.read(sz);
  for(size_t i=0;i<sz;++i){
    auto t = uint32_t(daedalus::DaedalusDataType::VOID);
    fin.read(t);
    switch(daedalus::DaedalusDataType(t)) {
      case daedalus::DaedalusDataType::INT:{
        fin.read(name);
        auto* s = findSymbol(name);
        if(s==nullptr && name.rfind(OwnSymbols::Prefix,0)==0) {
          // a save made before its symbols had an entry of their own: the director's story variable is ours now
          std::string why;
          s = own.addInt(name,why);
          }

        uint32_t size;
        fin.read(size);

        int v = 0;
        for(unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_int(v, uint16_t(j));
            }
          }

        break;
        }
      case daedalus::DaedalusDataType::FLOAT:{
        fin.read(name);
        auto* s = findSymbol(name);

        uint32_t size;
        fin.read(size);

        float v = 0;
        for (unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_float(v, uint16_t(j));
            }
          }

        break;
        }
      case daedalus::DaedalusDataType::STRING:{
        fin.read(name);
        auto* s = findSymbol(name);

        uint32_t size;
        fin.read(size);

        std::string v;
        for (unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_string(v, uint16_t(j));
            }
          }

        break;
        }
      case daedalus::DaedalusDataType::INSTANCE:{
        uint8_t dataClass=0;
        fin.read(dataClass);
        if(dataClass>0){
          uint32_t id=0;
          fin.read(name,id);
          auto* s = findSymbol(name);
          if (s == nullptr)
            break;
          if(dataClass==1) {
            auto npc = world().npcById(id);
            s->set_instance(npc ? npc->handlePtr() : nullptr);
            }
          else if(dataClass==2) {
            auto itm = world().itmById(id);
            s->set_instance(itm != nullptr ? itm->handlePtr() : nullptr);
            }
          else if(dataClass==3) {
            uint32_t itmClass=0;
            fin.read(itmClass);
            if(auto npc = world().npcById(id)) {
              auto itm = npc->getItem(itmClass);
              s->set_instance(itm ? itm->handlePtr() : nullptr);
              }
            }
          }
        break;
        }
      default:
        break;
      }
    }
  }

void GameScript::savePerc(Serialize& fout) {
  fout.write(uint32_t(PERC_Count));
  for(size_t i=0; i<PERC_Count; ++i) {
    fout.write(perceptionRanges.range[i]);
    }
  }

void GameScript::loadPerc(Serialize& fin) {
  uint32_t count = 0;
  fin.read(count);
  if(count!=PERC_Count) {
    if(hasSymbolName("initPerceptions"))
      vm.call_function("initPerceptions");
    return;
    }
  for(size_t i=0; i<PERC_Count; ++i)
    fin.read(perceptionRanges.range[i]);
  }

void GameScript::resetVarPointers() {
  for(uint32_t i=0;i<vm.symbols().size();++i){
    auto* s = vm.find_symbol_by_index(i); // never returns nullptr
    if(s->is_instance_of<daedalus::INpc>() || s->is_instance_of<daedalus::IItem>()){
      s->set_instance(nullptr);
      }
    }
  }

const QuestLog& GameScript::questLog() const {
  return quests;
  }

void GameScript::saveSym(Serialize &fout, daedalus::DaedalusSymbol& i) {
  auto& w = world();
  switch(i.type()) {
    case daedalus::DaedalusDataType::INT:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_int(uint16_t(j)));
        return;
        }
      break;
    case daedalus::DaedalusDataType::FLOAT:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_float(uint16_t(j)));
        return;
        }
      break;
    case daedalus::DaedalusDataType::STRING:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_string(uint16_t(j)));
        return;
        }
      break;
    case daedalus::DaedalusDataType::INSTANCE:
      fout.write(i.type());

      if(i.is_instance_of<daedalus::INpc>()){
        auto hnpc = reinterpret_cast<const daedalus::INpc*>(i.get_instance().get());
        auto npc  = reinterpret_cast<const Npc*>(hnpc==nullptr ? nullptr : hnpc->user_ptr);
        fout.write(uint8_t(1),i.name(),world().npcId(npc));
        }
      else if(i.is_instance_of<daedalus::IItem>()){
        auto     item = reinterpret_cast<const daedalus::IItem*>(i.get_instance().get());
        uint32_t id   = w.itmId(item);
        if(id!=uint32_t(-1) || item==nullptr) {
          fout.write(uint8_t(2),i.name(),id);
          } else {
          uint32_t idNpc = uint32_t(-1);
          for(uint32_t r=0; r<w.npcCount(); ++r) {
            auto& n = *w.npcById(r);
            if(n.itemCount(item->symbol_index())>0) {
              idNpc = r;
              fout.write(uint8_t(3),i.name(),idNpc,uint32_t(item->symbol_index()));
              break;
              }
            }
          if(idNpc==uint32_t(-1))
            fout.write(uint8_t(2),i.name(),uint32_t(-1));
          }
        }
      else if(i.is_instance_of<daedalus::IFocus>() ||
              i.is_instance_of<daedalus::IGuildValues>() ||
              i.is_instance_of<daedalus::IInfo>()) {
        fout.write(uint8_t(0));
        }
      else {
        fout.write(uint8_t(0));
        }
      return;
    default:
      break;
    }
  fout.write(uint32_t(daedalus::DaedalusDataType::VOID));
  }

void GameScript::fixNpcPosition(Npc& npc, float angle0, float distBias) {
  LoadProbe::Startup::Detail::Scope detail("fix_position");
  auto& dyn  = *world().physic();
  auto  pos0 = npc.position();

  if(!npc.hasCollision()) {
    auto ray = dyn.ray(pos0+Vec3(0,100,0), pos0+Vec3(0,-200,0));
    if(ray.hasCol && std::abs(pos0.y-ray.v.y) < 10.f) {
      npc.updateTransform();
      return;
      }
    }

  for(int r = 0; r<=800; r+=20) {
    for(int ang = 0; ang<360; ang+=30) {
      float a = float((float(ang)+angle0)*M_PI/180.0);
      float d = float(r)+distBias;
      auto  p = pos0+Vec3(std::cos(a)*d, 0, std::sin(a)*d);

      auto ray = dyn.ray(p+Vec3(0,100,0), p+Vec3(0,-1000,0));
      detail.work();
      if(!ray.hasCol)
        continue;
      p.y = ray.v.y;
      npc.setPosition(p);
      if(!npc.hasCollision()) {
        npc.updateTransform();
        return;
        }
      if(d==0) {
        // no need to loop multiple angles, with R of zero
        break;
        }
      }
    }

  // npc.setPosition(pos0);
  }

const World &GameScript::world() const {
  return *owner.world();
  }

World &GameScript::world() {
  return *owner.world();
  }

daedalus::IFocus GameScript::findFocus(std::string_view name) {
  auto id = vm.find_symbol_by_name(name);
  if(id==nullptr)
    return {};
  try {
    return *vm.init_instance<daedalus::IFocus>(id);
    }
  catch(const daedalus::DaedalusScriptError&) {
    return {};
    }
  }

void GameScript::storeItem(Item *itm) {
  auto* s = vm.global_item();
  if(itm!=nullptr) {
    s->set_instance(itm->handlePtr());
    } else {
    s->set_instance(nullptr);
    }
  }

daedalus::DaedalusSymbol* GameScript::findSymbol(std::string_view s) {
  return vm.find_symbol_by_name(s);
  }

daedalus::DaedalusSymbol* GameScript::findSymbol(const size_t s) {
  return vm.find_symbol_by_index(uint32_t(s));
  }

size_t GameScript::findSymbolIndex(std::string_view name) {
  auto sym = vm.find_symbol_by_name(name);
  return sym == nullptr ? size_t(-1) : sym->index();
  }

size_t GameScript::symbolsCount() const {
  return vm.symbols().size();
  }

const AiState& GameScript::aiState(ScriptFn id) {
  auto it = aiStates.find(id.ptr);
  if(it!=aiStates.end())
    return it->second;
  auto ins = aiStates.emplace(id.ptr,AiState(*this,id.ptr));
  return ins.first->second;
  }

const daedalus::ISpell& GameScript::spellDesc(int32_t splId) {
  auto& tag = spellFxInstanceNames->get_string(uint16_t(splId));
  return spells->find(tag);
  }

const VisualFx* GameScript::spellVfx(int32_t splId) {
  auto& tag = spellFxInstanceNames->get_string(uint16_t(splId));
  string_frm name("spellFX_",tag);
  return Gothic::inst().loadVisualFx(name);
  }

GameScript::PhysicalSpell GameScript::physicalSpell(int32_t splId) const {
  if(spellFxInstanceNames==nullptr || splId<0 || size_t(splId)>=spellFxInstanceNames->count())
    return PhysicalSpell::None;
  auto tag = spellFxInstanceNames->get_string(uint16_t(splId));
  for(auto& c:tag)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  if(tag=="WINDFIST" || tag=="STORMFIST")
    return PhysicalSpell::WindFist;
  if(tag=="WHIRLWIND")
    return PhysicalSpell::Whirlwind;
  if(tag=="INSTANTFIREBALL" || tag=="CHARGEFIREBALL" || tag=="FIREBALL")
    return PhysicalSpell::Fireball;
  if(tag=="ICECUBE" || tag=="ICEBLOCK")
    return PhysicalSpell::IceCube;
  return PhysicalSpell::None;
  }

std::vector<GameScript::DlgChoice> GameScript::dialogChoices(std::shared_ptr<daedalus::INpc> player,
                                                             std::shared_ptr<daedalus::INpc> hnpc,
                                                             const std::vector<uint32_t>& except,
                                                             bool includeImp) {
  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), player);
  std::vector<daedalus::IInfo*> hDialog;
  for(auto& info : dialogsInfo) {
    if(info->npc==static_cast<int>(hnpc->symbol_index())) {
      hDialog.push_back(info.get());
      }
    }

  std::vector<DlgChoice> choice;
  for(int important=includeImp ? 1 : 0;important>=0;--important){
    for(auto& i:hDialog) {
      const daedalus::IInfo& info = *i;
      if(info.important!=important)
        continue;
      bool npcKnowsInfo = doesNpcKnowInfo(*player,vm.find_symbol_by_instance(info)->index());
      if(npcKnowsInfo && !info.permanent)
        continue;

      if(info.important && info.permanent){
        bool skip=false;
        for(auto i:except)
          if(static_cast<int>(i)==info.information){
            skip=true;
            break;
            }
        if(skip)
          continue;
        }

      bool valid=true;
      if(info.condition) {
        auto* conditionSymbol = vm.find_symbol_by_index(uint32_t(info.condition));
        if (conditionSymbol != nullptr) {
          valid = dialogFunction(conditionSymbol,true) != 0;
          }
        }
      if(Gothic::inst().dialogFilterFn)
        Gothic::inst().dialogFilterFn(info,valid);
      if(!valid)
        continue;

      DlgChoice ch;
      ch.title    = info.description;
      ch.scriptFn = uint32_t(info.information);
      ch.handle   = i;
      ch.isTrade  = info.trade!=0;
      ch.sort     = info.nr;
      choice.emplace_back(std::move(ch));
      }
    if(Gothic::inst().dialogExtraFn)
      Gothic::inst().dialogExtraFn(*hnpc,important!=0,choice);
    if(!choice.empty()){
      sort(choice);
      return choice;
      }
    }
  sort(choice);
  return choice;
  }

std::vector<GameScript::DlgChoice> GameScript::updateDialog(const GameScript::DlgChoice &dlg, Npc& player,Npc& npc) {
  std::vector<GameScript::DlgChoice> ret;
  if(dlg.handle==nullptr) {
    // made at runtime (ai_bridge): its choices, if it has any
    if(!dlg.external.empty() && Gothic::inst().dialogExtraChoicesFn)
      Gothic::inst().dialogExtraChoicesFn(dlg,ret);
    sort(ret);
    return ret;
    }
  const daedalus::IInfo&               info = *dlg.handle;

  ScopeVar self (*vm.global_self(), npc.handlePtr());
  ScopeVar other(*vm.global_other(), player.handlePtr());

  for(size_t i=0;i<info.choices.size();++i){
    auto& sub = info.choices[i];
    if(Gothic::inst().dialogChoiceFilterFn && !Gothic::inst().dialogChoiceFilterFn(info,sub.function))
      continue;
    GameScript::DlgChoice ch;
    ch.title    = sub.text;
    ch.scriptFn = uint32_t(sub.function);
    ch.handle   = dlg.handle;
    ch.isTrade  = false;
    ch.sort     = int(i);
    ret.push_back(ch);
    }

  sort(ret);
  return ret;
  }

void GameScript::exec(const GameScript::DlgChoice &dlg, Npc& player, Npc& npc) {
  ScopeVar self (*vm.global_self(), npc.handlePtr());
  ScopeVar other(*vm.global_other(), player.handlePtr());

  if(dlg.handle==nullptr) {
    if(&player!=&npc)
      player.stopAnim("");
    if(Gothic::inst().dialogExtraExecFn)
      Gothic::inst().dialogExtraExecFn(dlg,player,npc);
    Gothic::inst().onDialogChosen(dlg,player,npc);
    return;
    }
  daedalus::IInfo& info = *dlg.handle;

  if(&player!=&npc)
    player.stopAnim("");
  auto& pl = player.handle();

  if(info.information==int(dlg.scriptFn)) {
    setNpcInfoKnown(pl,info);
    } else {
    for(size_t i=0;i<info.choices.size();){
      if(info.choices[i].function==int(dlg.scriptFn))
        info.choices.erase(info.choices.begin()+int(i)); else
        ++i;
      }
    }
  auto* function = vm.find_symbol_by_index(dlg.scriptFn);
  std::shared_ptr<HeroDeed> pickpocket;
  if(function!=nullptr && function->name().ends_with("_DOIT") &&
     (function->name().find("PICKPOCKET")!=std::string::npos || function->name()=="DIA_CASSIA_PICKME_DOIT")) {
    pickpocket = HeroDeed::capture(player,"pickpocket",&npc);
    if(pickpocket!=nullptr) {
      pickpocket->inventoryBefore = HeroDeed::inventory(player);
      pickpocket->pickedPocketBefore = HeroDeed::aivar(npc,"AIV_PLAYERHASPICKEDMYPOCKET");
      }
    }
  dialogFunction(function,false);
  Gothic::inst().onDialogChosen(dlg,player,npc);
  if(pickpocket!=nullptr) {
    pickpocket->inventoryAfter = HeroDeed::inventory(player);
    pickpocket->pickedPocketAfter = HeroDeed::aivar(npc,"AIV_PLAYERHASPICKEDMYPOCKET");
    pickpocket->caught = HeroDeed::aivar(npc,"AIV_ATTACKREASON")==HeroDeed::constant(npc,"AR_Theft");
    if(pickpocket->caught)
      pickpocket->witnessed(npc);
    Gothic::inst().onHeroDeed(*pickpocket);
    }
  }

int32_t GameScript::dialogFunction(daedalus::DaedalusSymbol* fn, bool returns) {
  int32_t r = 0;
  if(fn!=nullptr && Gothic::inst().dialogScriptFn && Gothic::inst().dialogScriptFn(*fn,r))
    return r;
  if(returns)
    return vm.call_function<int>(fn);
  vm.call_function(fn);
  return 0;
  }

void GameScript::printCannotUseError(Npc& npc, int32_t atr, int32_t nValue) {
  auto id = vm.find_symbol_by_name("G_CanNotUse");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id, npc.isPlayer(), atr, nValue);
  }

void GameScript::printCannotCastError(Npc &npc, int32_t plM, int32_t itM) {
  auto id = vm.find_symbol_by_name("G_CanNotCast");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id, npc.isPlayer(), itM, plM);
  }

void GameScript::printCannotBuyError(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_trade_not_enough_gold");
  if(id==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingItem(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_item");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKey(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobAnotherIsUsing(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_another_is_using");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKeyOrLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key_or_lockpick");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_lockpick");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobTooFar(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_too_far_away");
  if(id==nullptr) {
    owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::invokeState(const std::shared_ptr<daedalus::INpc>& hnpc, const std::shared_ptr<daedalus::INpc>& oth, const char *name) {
  auto id = vm.find_symbol_by_name(name);
  if(id==nullptr)
    return;

  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), oth);
  vm.call_function<void>(id);
  }

int GameScript::invokeState(Npc* npc, Npc* oth, Npc* vic, ScriptFn fn) {
  if(!fn.isValid())
    return 0;
  if(oth==nullptr){
    // oth=npc; //FIXME: PC_Levelinspektor?
    }
  if(vic==nullptr){
    // vic=owner.player();
    }

  if(fn==ZS_Talk){
    if(oth==nullptr || !oth->isPlayer()) {
      Log::e("unxepected perc acton");
      return 0;
      }
    }

  ScopeCtx ctx   (*this, npc!=nullptr ? npc->processPolicy() : NpcProcessPolicy::AiNormal);
  ScopeVar self  (*vm.global_self(),   npc != nullptr ? npc->handlePtr() : nullptr);
  ScopeVar other (*vm.global_other(),  oth != nullptr ? oth->handlePtr() : nullptr);
  ScopeVar victim(*vm.global_victim(), vic != nullptr ? vic->handlePtr() : nullptr);

  auto* sym = vm.find_symbol_by_index(uint32_t(fn.ptr));
  int   ret = 0;
  if(sym!=nullptr && sym->rtype() == daedalus::DaedalusDataType::INT) {
    ret = vm.call_function<int>(sym);
    }
  else if(sym!=nullptr) {
    vm.call_function<void>(sym);
    ret = 0;
    }

  if(vm.global_other()->is_instance_of<daedalus::INpc>()){
    auto oth2 = reinterpret_cast<daedalus::INpc*>(vm.global_other()->get_instance().get());
    if(oth!=nullptr && oth2!=&oth->handle()) {
      Npc* other = findNpc(oth2);
      npc->setOther(other);
      }
    }
  return ret;
  }

void GameScript::invokeItem(Npc *npc, ScriptFn fn) {
  if(fn==size_t(-1) || fn == 0)
    return;
  auto functionSymbol = vm.find_symbol_by_index(uint32_t(fn.ptr));

  if (functionSymbol == nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc->handlePtr());
  vm.call_function<void>(functionSymbol);
  }

int GameScript::invokeMana(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  return vm.call_function<int>(fn,mana);
  }

int GameScript::invokeManaRelease(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana_Release");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  return vm.call_function<int>(fn,mana);
  }

void GameScript::invokeSpell(Npc &npc, Npc* target, Item &it) {
  auto&      tag = spellFxInstanceNames->get_string(uint16_t(it.spellId()));
  string_frm name("Spell_Cast_",tag);
  auto       fn = vm.find_symbol_by_name(name);
  if(fn==nullptr)
    return;

  // FIXME: actually set the spell level!
  int32_t  splLevel = 0;
  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);
  try {
    if(fn->count()==1) {
      // this is a leveled spell
      vm.call_function<void>(fn, splLevel);
      } else {
      vm.call_function<void>(fn);
      }
    }
  catch(...) {
    Log::d("unable to call spell-script: \"",name,"\'");
    }
  }

int GameScript::invokeCond(Npc& npc, std::string_view func) {
  auto fn = vm.find_symbol_by_name(func);
  if(fn==nullptr) {
    Gothic::inst().onPrint("MOBSI::conditionFunc is not invalid");
    return 1;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  return vm.call_function<int>(fn);
  }

void GameScript::invokePickLock(Npc& npc, int bSuccess, int bBrokenOpen) {
  auto fn   = vm.find_symbol_by_name("G_PickLock");
  if(fn==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(fn, bSuccess, bBrokenOpen);
  }

void GameScript::invokeRefreshAtInsert(Npc& npc) {
  LoadProbe::Startup::Detail::Scope detail("refresh_at_insert");
  if(B_RefreshAtInsert==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(B_RefreshAtInsert);
  }

CollideMask GameScript::canNpcCollideWithSpell(Npc& npc, Npc* shooter, int32_t spellId) {
  auto fn   = vm.find_symbol_by_name("C_CanNpcCollideWithSpell");
  if(fn==nullptr)
    return COLL_DOEVERYTHING;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), shooter->handlePtr());
  return CollideMask(vm.call_function<int>(fn, spellId));
  }

int GameScript::playerHotKeyScreenMap(Npc& pl) {
  auto fn   = vm.find_symbol_by_name("player_hotkey_screen_map");
  if(fn==nullptr) {
    return -1;
    }

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  int map = vm.call_function<int>(fn);
  if(map>=0)
    pl.useItem(size_t(map));
  return map;
  }

void GameScript::playerHotLamePotion(Npc& pl) {
  auto opt = Gothic::inst().settingsGetI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_potion");
  if(fn==nullptr)
    return;

  if(Gothic::physicsFlags().meleeHealInterrupt && startHotkeyPotion(pl,true))
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  vm.call_function<void>(fn);
  }

void GameScript::playerHotLameHeal(Npc& pl) {
  auto opt = Gothic::inst().settingsGetI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_heal");
  if(fn==nullptr)
    return;

  if(Gothic::physicsFlags().meleeHealInterrupt && startHotkeyPotion(pl,false))
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  vm.call_function<void>(fn);
  }

bool GameScript::startHotkeyPotion(Npc& pl, bool mana) {
  const char* kind = mana ? "mana" : "health";
  auto note = [kind](std::string_view item) {
    if(TestBench::isActive())
      TestBench::note(string_frm("heal hotkey ",kind,": ",item).c_str());
    };
  if(pl.isDown()) {
    note("none");
    return true;
    }
  if(pl.hasState(BS_ITEMINTERACT) || pl.hasQueuedItemUse())
    return true;

  const char* healthItems[] = {"ITPO_HEALTH_01","ITPO_HEALTH_02","ITPO_HEALTH_03"};
  const char* manaItems[] = {"ITPO_MANA_01","ITPO_MANA_02","ITPO_MANA_03"};
  const char* healthAmounts[] = {"HP_ESSENZ","HP_EXTRAKT","HP_ELIXIER"};
  const char* manaAmounts[] = {"MANA_ESSENZ","MANA_EXTRAKT","MANA_ELIXIER"};
  Healing::Potion candidates[3] = {};
  for(size_t i=0; i<3; ++i) {
    auto* item = vm.find_symbol_by_name(mana ? manaItems[i] : healthItems[i]);
    auto* amount = vm.find_symbol_by_name(mana ? manaAmounts[i] : healthAmounts[i]);
    if(item==nullptr || amount==nullptr)
      throw std::runtime_error("Missing potion hotkey item or amount symbol");
    candidates[i] = {item->index(),amount->get_int(),pl.itemCount(item->index())>0};
    }
  const auto missing = pl.attribute(mana ? ATR_MANAMAX : ATR_HITPOINTSMAX) -
                       pl.attribute(mana ? ATR_MANA : ATR_HITPOINTS);
  // PLAYER_HOTKEY_LAME_* uses the strongest dose that fits the missing amount,
  // otherwise the weakest available dose. Its no-item/full-gauge messages still run below.
  const auto selected = Healing::choosePotion(candidates,missing);
  if(selected==Healing::NoPotion) {
    note("none");
    return false;
    }
  note(vm.find_symbol_by_index(uint32_t(selected))->name());
  if(pl.weaponState()!=WeaponState::NoWeapon && pl.itemUseInHandAnimation(*pl.getItem(selected)).empty()) {
    if(TestBench::isActive())
      TestBench::note(("item use "+std::string(TestBench::who(&pl))+": "+
                      vm.find_symbol_by_index(uint32_t(selected))->name()+" holster path: no compatible in-hand clip or deferred effect").c_str());
    pl.stopAnim("");
    pl.aiPush(AiQueue::aiRemoveWeapon());
    }
  pl.aiPush(AiQueue::aiUseItem(int32_t(selected)));
  return true;
  }

std::string_view GameScript::spellCastAnim(Npc&, Item &it) {
  if(spellFxAniLetters==nullptr)
    return "FIB";
  return spellFxAniLetters->get_string(uint16_t(it.spellId()));
  }

bool GameScript::aiOutput(Npc &npc, std::string_view outputname, bool overlay) {
  string_frm name(outputname,".WAV");
  uint64_t   dt = 0;

  std::shared_ptr<LipSync::Track> voice;
  world().addDlgSound(name, npc.mapHeadBone(), WorldSound::talkRange, dt, &voice);
  npc.startSpeech(std::move(voice));
  npc.setAiOutputBarrier(messageTime(outputname),overlay);

  return true;
  }

bool GameScript::aiOutputSvm(Npc &npc, std::string_view outputname, std::string_view svm, Npc* target, bool overlay) {
  if(overlay) {
    if(tickCount()<svmBarrier)
      return true;
    svmBarrier = tickCount()+messageTime(outputname);
    }

  if(!outputname.empty()) {
    if(auto& fn = Gothic::inst().interceptSvm; fn && fn(npc,svm,outputname,target,overlay)) {
      npc.setAiOutputBarrier(messageTime(outputname),overlay);
      return true;
      }
    return aiOutput(npc,outputname,overlay);
    }
  return true;
  }

bool GameScript::isDead(const Npc &pl) {
  return pl.isInState(ZS_Dead);
  }

bool GameScript::isUnconscious(const Npc &pl) {
  return pl.isInState(ZS_Unconscious);
  }

void GameScript::skipImportantInfos(Npc& npc, Npc& pl) {
  for(auto& info:dialogsInfo)
    if(info->npc==int32_t(npc.handle().symbol_index()) && info->important && !info->permanent)
      setNpcInfoKnown(pl.handle(),*info);
  }

bool GameScript::isTalk(const Npc &pl) {
  return pl.isInState(ZS_Talk);
  }

bool GameScript::isAttack(const Npc& pl) const {
  return pl.isInState(ZS_Attack) || pl.isInState(ZS_MM_Attack);
  }

std::string_view GameScript::messageFromSvm(std::string_view id, int voice) const {
  return svm->find(id,voice);
  }

std::string_view GameScript::messageByName(std::string_view id) const {
  std::string_view ext;
  uint32_t         extTime = 0;
  if(auto& fn = Gothic::inst().externalMessageFn; fn && fn(id,ext,extTime))
    return ext;

  auto blk = OutputUnits::blockByName(dialogs,id);
  if(blk == nullptr)
    return "";
  auto msg = OutputUnits::message(*blk);
  if(msg == nullptr)
    return "";
  return msg->text;
  }

uint32_t GameScript::messageTime(std::string_view id) const {
  std::string_view ext;
  uint32_t         extTime = 0;
  if(auto& fn = Gothic::inst().externalMessageFn; fn && fn(id,ext,extTime))
    return extTime;

  static std::string tmp;
  tmp.assign(id);

  uint32_t& time = msgTimings[tmp];
  if(time>0)
    return time;

  string_frm name(id,".WAV");
  auto s = Resources::loadSoundBuffer(name);
  if(s.timeLength()>0) {
    time = uint32_t(s.timeLength());
    } else {
    auto txt = messageByName(id);
    time = uint32_t(float(txt.length())*viewTimePerChar);
    time = std::min(time, 16000u);
    }
  return time;
  }

void GameScript::printNothingToGet() {
  auto id = vm.find_symbol_by_name("player_plunder_is_empty");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), owner.player()->handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::useInteractive(const std::shared_ptr<daedalus::INpc>& hnpc, std::string_view func) {
  auto fn = vm.find_symbol_by_name(func);
  if(fn == nullptr)
    return;

  ScopeVar self(*vm.global_self(),hnpc);
  try {
    vm.call_function<void>(fn);
    }
  catch (...) {
    Log::i("unable to use interactive [",func,"]");
    }
  }

Attitude GameScript::guildAttitude(const Npc &p0, const Npc &p1) const {
  // The approach stand's single target must wander without becoming a combat
  // encounter. Preserve its real guild, including all locomotion parameters.
  if(NavDemos::nonhostileTarget(p0) || NavDemos::nonhostileTarget(p1))
    return ATT_NEUTRAL;
  auto selfG = std::min<size_t>(gilCount-1,p0.guild());
  auto npcG  = std::min<size_t>(gilCount-1,p1.guild());
  auto ret   = gilAttitudes[selfG*gilCount+npcG];
  return Attitude(ret);
  }

Attitude GameScript::personAttitude(const Npc &p0, const Npc &p1) const {
  if(NavDemos::nonhostileTarget(p0) || NavDemos::nonhostileTarget(p1))
    return ATT_NEUTRAL;
  if(!p0.isPlayer() && !p1.isPlayer())
    return guildAttitude(p0,p1);

  Attitude att=ATT_NULL;
  const Npc& npc = p0.isPlayer() ? p1 : p0;
  att = npc.attitude();
  if(att!=ATT_NULL)
    return att;
  att = guildAttitude(p0,p1);
  return att;
  }

bool GameScript::isFriendlyFire(const Npc& src, const Npc& dst) const {
  const int AIV_PARTYMEMBER = 15;
  if(src.isPlayer())
    return false;
  if(personAttitude(src, dst)==ATT_FRIENDLY)
    return true;
  if(src.handlePtr()->aivar[AIV_PARTYMEMBER]!=0 && dst.isPlayer())
    return true;
  return false;
  }

BodyState GameScript::schemeToBodystate(std::string_view sc) {
  if(searchScheme(sc,"MOB_SIT"))
    return BS_SIT;
  if(searchScheme(sc,"MOB_LIE"))
    return BS_LIE;
  if(searchScheme(sc,"MOB_CLIMB"))
    return BS_CLIMB;
  if(searchScheme(sc,"MOB_NOTINTERRUPTABLE"))
    return BS_MOBINTERACT;
  return BS_MOBINTERACT_INTERRUPT;
  }

void GameScript::onWldItemRemoved(const Item& itm) {
  onWldInstanceRemoved(&itm.handle());
  }

void GameScript::onWldInstanceRemoved(const daedalus::DaedalusInstance* obj) {
  vm.find_symbol_by_instance(*obj)->set_instance(nullptr);
  }

void GameScript::makeCurrent(Item* w) {
  if(w==nullptr)
    return;
  auto* s = vm.find_symbol_by_index(uint32_t(w->clsId()));
  if(s != nullptr)
    s->set_instance(w->handlePtr());
  }

bool GameScript::searchScheme(std::string_view sc, std::string_view listName) {
  std::string_view list = findSymbol(listName)->get_string();
  for(size_t i=0; i<=list.size(); ++i) {
    if(i==list.size() || list[i]==',') {
      if(sc==list.substr(0,i))
        return true;
      if(i==list.size())
         break;
      list = list.substr(i+1);
      i    = 0;
      }
    }
  return false;
  }

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic) {
  auto lang   = gothic.settingsGetI("GAME", "language");
  auto script = gothic.loadScript(gothic.defaultGameDatFile(), ScriptLang(lang));
  auto exef   = daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  return daedalus::DaedalusVm(std::move(script), exef);
  }

bool GameScript::hasSymbolName(std::string_view name) {
  return vm.find_symbol_by_name(name)!=nullptr;
  }

uint64_t GameScript::tickCount() const {
  return owner.tickCount();
  }

void GameScript::tick(uint64_t) {
  }

uint32_t GameScript::rand(uint32_t max, const Npc* actor) {
  if(actor!=nullptr && !isolatedRandom.empty()) {
    auto it = isolatedRandom.find(actor);
    if(it!=isolatedRandom.end())
      return uint32_t(it->second())%max;
    }
  return uint32_t(randGen())%max;
  }

void GameScript::isolateRandom(const Npc& actor, uint32_t seed, uint32_t stream) {
  std::seed_seq sequence{seed, stream, 0x5C21D7u};
  isolatedRandom[&actor].seed(sequence);
  }

std::string GameScript::randomState(const Npc* actor) const {
  std::ostringstream out;
  auto it = actor==nullptr ? isolatedRandom.end() : isolatedRandom.find(actor);
  if(it!=isolatedRandom.end())
    out << it->second; else
    out << randGen;
  return out.str();
  }

Npc* GameScript::findNpc(daedalus::DaedalusSymbol* s) {
  if(s->is_instance_of<daedalus::INpc>()) {
    auto cNpc = reinterpret_cast<daedalus::INpc*>(s->get_instance().get());
    return findNpc(cNpc);
    }
  return nullptr;
  }

Npc* GameScript::findNpc(daedalus::INpc *handle) {
  if(handle==nullptr)
    return nullptr;
  assert(handle->user_ptr); // engine bug, if null
  return reinterpret_cast<Npc*>(handle->user_ptr);
  }

Npc* GameScript::findNpc(const std::shared_ptr<daedalus::INpc>& handle) {
  if(handle==nullptr)
    return nullptr;
  assert(handle->user_ptr); // engine bug, if null
  return reinterpret_cast<Npc*>(handle->user_ptr);
  }

Npc* GameScript::findNpcById(const std::shared_ptr<daedalus::DaedalusInstance>& handle) {
  if(handle==nullptr)
    return nullptr;
  if(auto npc = dynamic_cast<const daedalus::INpc*>(handle.get())) {
    assert(npc->user_ptr); // engine bug, if null
    return reinterpret_cast<Npc*>(npc->user_ptr);
    }
  return findNpcById(handle->symbol_index());
  }

Item *GameScript::findItem(daedalus::IItem* handle) {
  if(handle==nullptr)
    return nullptr;
  auto& itData = *handle;
  assert(itData.user_ptr); // engine bug, if null
  return reinterpret_cast<Item*>(itData.user_ptr);
  }

Item *GameScript::findItemById(size_t id) {
  auto* handle = vm.find_symbol_by_index(uint32_t(id));
  if(handle==nullptr||!handle->is_instance_of<daedalus::IItem>())
    return nullptr;
  auto hitm = reinterpret_cast<daedalus::IItem*>(handle->get_instance().get());
  return findItem(hitm);
  }

Item* GameScript::findItemById(const std::shared_ptr<daedalus::DaedalusInstance>& handle) {
  if(handle==nullptr)
    return nullptr;
  if(auto itm = dynamic_cast<const daedalus::IItem*>(handle.get())) {
    assert(itm->user_ptr); // engine bug, if null
    return reinterpret_cast<Item*>(itm->user_ptr);
    }
  return findItemById(handle->symbol_index());
  }

Npc* GameScript::findNpcById(size_t id) {
  auto* handle = vm.find_symbol_by_index(uint32_t(id));
  if(handle==nullptr || !handle->is_instance_of<daedalus::INpc>())
    return nullptr;

  auto hnpc = reinterpret_cast<daedalus::INpc*>(handle->get_instance().get());
  if(hnpc==nullptr) {
    auto obj = world().findNpcByInstance(id);
    handle->set_instance(obj ? obj->handlePtr() : nullptr);
    hnpc = reinterpret_cast<daedalus::INpc*>(handle->get_instance().get());
    }
  return findNpc(hnpc);
  }

daedalus::IInfo* GameScript::findInfo(size_t id) {
  auto* sym = vm.find_symbol_by_index(uint32_t(id));
  if(sym==nullptr||!sym->is_instance_of<daedalus::IInfo>())
    return nullptr;
  auto* h = sym->get_instance().get();
  if(h==nullptr)
    Log::e("invalid c_info object: \"",sym->name(),"\"");
  return reinterpret_cast<daedalus::IInfo*>(h);
  }

void GameScript::removeItem(Item &it) {
  world().removeItem(it);
  }

void GameScript::setInstanceNPC(std::string_view name, Npc &npc) {
  auto sym = vm.find_symbol_by_name(name);
  if(sym == nullptr) {
    Tempest::Log::e("Cannot set NPC instance ", name, ": Symbol not found.");
    return;
    }
  sym->set_instance(npc.handlePtr());
  }

void GameScript::setInstanceItem(Npc &holder, size_t itemId) {
  storeItem(holder.getItem(itemId));
  }

AiOuputPipe *GameScript::openAiOuput() {
  return aiDefaultPipe.get();
  }

AiOuputPipe *GameScript::openDlgOuput(Npc &player, Npc &npc) {
  if(player.isPlayer())
    return owner.openDlgOuput(player,npc);
  return owner.openDlgOuput(npc,player);
  }

ScriptFn GameScript::playerPercAssessMagic() {
  auto id = vm.find_symbol_by_name("PLAYER_PERC_ASSESSMAGIC");
  if(id==nullptr)
    return ScriptFn();

  if(id->count()>0)
    return ScriptFn(uint32_t(id->get_int()));
  return ScriptFn();
  }

int GameScript::npcDamDiveTime() {
  auto id = vm.find_symbol_by_name("NPC_DAM_DIVE_TIME");
  if(id==nullptr)
    return 0;
  return id->get_int();
  }

uint32_t GameScript::lockPickId() const {
  return ItKE_lockpick!=nullptr ? ItKE_lockpick->index() : 0;
  }

std::string_view GameScript::menuMain() const {
  return ::menuMain;
  }

bool GameScript::game_initgerman() {
  return true;
  }

bool GameScript::game_initenglish() {
  return true;
  }

void GameScript::wld_settime(int hour, int minute) {
  world().setDayTime(hour,minute);
  }

int GameScript::wld_getday() {
  return int(owner.time().day());
  }

void GameScript::wld_playeffect(std::string_view visual, std::shared_ptr<daedalus::DaedalusInstance> sourceId, std::shared_ptr<daedalus::DaedalusInstance> targetId,
                                int effectLevel, int damage, int damageType, int isProjectile) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  if(isProjectile!=0 || damageType!=0 || damage!=0 || effectLevel!=0) {
    // TODO
    Log::i("effect not implemented [",visual.data(),"]");
    return;
    }
  const VisualFx* vfx = Gothic::inst().loadVisualFx(visual);
  if(vfx==nullptr) {
    Log::i("invalid effect [",visual.data(),"]");
    return;
    }

  auto dstNpc = findNpcById(targetId);
  auto srcNpc = findNpcById(sourceId);

  auto dstItm = findItemById(targetId);
  auto srcItm = findItemById(sourceId);

  if(srcNpc!=nullptr && dstNpc!=nullptr) {
    srcNpc->startEffect(*dstNpc,*vfx);
    } else
  if(srcItm!=nullptr && dstItm!=nullptr){
    Effect e(*vfx,world(),srcItm->position());
    e.setActive(true);
    world().runEffect(std::move(e));
    }
  }

void GameScript::wld_stopeffect(std::string_view visual) {
  const VisualFx*          vfx    = Gothic::inst().loadVisualFx(visual);
  if(vfx==nullptr) {
    Log::i("invalid effect [",visual.data(),"]");
    return;
    }
  if(auto w = owner.world())
    w->stopEffect(*vfx);
  }

int GameScript::wld_getplayerportalguild() {
  int32_t g = GIL_NONE;
  if(auto p = world().player())
    g = world().guildOfRoom(p->portalName());
  return g;
  }

int GameScript::wld_getformerplayerportalguild() {
  int32_t g = GIL_NONE;
  if(auto p = world().player())
    g = world().guildOfRoom(p->formerPortalName());
  return g;
  }

void GameScript::wld_setguildattitude(int gil1, int att, int gil2) {
  if(gil1<0 || gil2<0 || gil1>=int(gilCount) || gil2>=int(gilCount))
    return;
  gilAttitudes[size_t(gil1)*gilCount+size_t(gil2)] = att;
  }

int GameScript::wld_getguildattitude(int gil1, int gil2) {
  if(gil1<0 || gil2<0 || gil1>=int(gilCount) || gil2>=int(gilCount))
    return ATT_HOSTILE; // error
  return gilAttitudes[size_t(gil1)*gilCount+size_t(gil2)];
  }

void GameScript::wld_exchangeguildattitudes(std::string_view name) {
  auto guilds = vm.find_symbol_by_name(name);
  if(guilds==nullptr)
    return;
  for(size_t i=0;i<gilTblSize;++i) {
    for(size_t r=0;r<gilTblSize;++r)
      gilAttitudes[i*gilCount+r] = guilds->get_int(uint16_t(i * gilTblSize + r));
    }
  }

bool GameScript::wld_istime(int hour0, int min0, int hour1, int min1) {
  gtime begin{hour0,min0}, end{hour1,min1};
  gtime now = owner.time();
  now = gtime(0,now.hour(),now.minute());

  if(begin<=end && begin<=now && now<end)
    return true;
  else if(end<begin && (now<end || begin<=now))
    return true;
  else
    return 0;
  }

bool GameScript::wld_isfpavailable(std::shared_ptr<daedalus::INpc> self, std::string_view name) {
  if(self==nullptr){
    return false;
    }

  auto wp = world().findFreePoint(*findNpc(self.get()),name);
  return wp!=nullptr;
  }

bool GameScript::wld_isnextfpavailable(std::shared_ptr<daedalus::INpc> self, std::string_view name) {
  if(self==nullptr){
    return false;
    }
  auto fp = world().findNextFreePoint(*findNpc(self.get()),name);
  return fp != nullptr;
  }

bool GameScript::wld_ismobavailable(std::shared_ptr<daedalus::INpc> self, std::string_view name) {
  auto npc = findNpc(self);
  if(npc==nullptr) {
    return false;
    }

  auto wp = world().availableMob(*npc, name);
  return wp != nullptr;
  }

void GameScript::wld_setmobroutine(int h, int m, std::string_view name, int st) {
  world().setMobRoutine(gtime(h,m), name, st);
  }

int GameScript::wld_getmobstate(std::shared_ptr<daedalus::INpc> npcRef, std::string_view scheme) {
  auto npc = findNpc(npcRef);

  if(npc==nullptr) {
    return -1;
    }

  auto mob = world().availableMob(*npc,scheme);
  if(mob==nullptr) {
    return -1;
    }

  return std::max(0,mob->stateId());
  }

void GameScript::wld_assignroomtoguild(std::string_view name, int g) {
  world().assignRoomToGuild(name,g);
  }

bool GameScript::wld_detectnpc(std::shared_ptr<daedalus::INpc> npcRef, int inst, int state, int guild) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }

  Npc*  ret =nullptr;
  float dist=std::numeric_limits<float>::max();

  world().detectNpc(npc->position(), float(npc->handle().senses_range), [inst,state,guild,&ret,&dist,npc](Npc& n){
    if((inst ==-1 || int32_t(n.instanceSymbol())==inst) &&
       (state==-1 || n.isInState(uint32_t(state))) &&
       (guild==-1 || int32_t(n.guild())==guild) &&
       (&n!=npc) && !n.isDead()) {
      float d = n.qDistTo(*npc);
      if(d<dist){
        ret = &n;
        dist = d;
        }
      }
    });
  if(ret)
    vm.global_other()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

bool GameScript::wld_detectnpcex(std::shared_ptr<daedalus::INpc> npcRef, int inst, int state, int guild, int player) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }
  Npc*  ret =nullptr;
  float dist=std::numeric_limits<float>::max();

  world().detectNpc(npc->position(), float(npc->handle().senses_range), [inst,state,guild,&ret,&dist,npc,player](Npc& n){
    if((inst ==-1 || int32_t(n.instanceSymbol())==inst) &&
       (state==-1 || n.isInState(uint32_t(state))) &&
       (guild==-1 || int32_t(n.guild())==guild) &&
       (&n!=npc) && !n.isDead() &&
       (player!=0 || !n.isPlayer())) {
      float d = n.qDistTo(*npc);
      if(d<dist){
        ret = &n;
        dist = d;
        }
      }
    });
  if(ret)
    vm.global_other()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

bool GameScript::wld_detectitem(std::shared_ptr<daedalus::INpc> npcRef, int flags) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }

  Item* ret =nullptr;
  float dist=std::numeric_limits<float>::max();
  world().detectItem(npc->position(), float(npc->handle().senses_range), [npc,&ret,&dist,flags](Item& it) {
    if((it.handle().main_flag&flags)==0)
      return;
    float d = (npc->position()-it.position()).quadLength();
    if(d<dist) {
      ret = &it;
      dist= d;
      }
    });

  if(ret)
    vm.global_item()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

void GameScript::wld_spawnnpcrange(std::shared_ptr<daedalus::INpc> npcRef, int clsId, int count, float lifeTime) {
  auto at = findNpc(npcRef);
  if(at==nullptr || clsId<=0)
    return;

  (void)lifeTime;
  for(int32_t i=0;i<count;++i) {
    auto* npc = world().addNpc(size_t(clsId),at->position());
    fixNpcPosition(*npc,at->rotation() + 360.f*float(i)/float(count),100);
    }
  }

void GameScript::wld_sendtrigger(std::string_view triggerTarget) {
  if(triggerTarget.empty())
    return;
  auto& world = *owner.world();
  const TriggerEvent evt(std::string{triggerTarget},"",world.tickCount(),TriggerEvent::T_Trigger);
  world.triggerEvent(evt);
  }

void GameScript::wld_senduntrigger(std::string_view triggerTarget) {
  if(triggerTarget.empty())
    return;
  auto& world = *owner.world();
  const TriggerEvent evt(std::string{triggerTarget},"",world.tickCount(),TriggerEvent::T_Untrigger);
  world.triggerEvent(evt);
  }

bool GameScript::wld_israining() {
  static bool first=true;
  if(first){
    Log::e("not implemented call [wld_israining]");
    first=false;
  }
  return false;
  }

void GameScript::mdl_setvisual(std::shared_ptr<daedalus::INpc> npcRef, std::string_view visual) {
  LoadProbe::Startup::Detail::Scope detail("mdl_setvisual",[&]{return std::string(visual);});
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;
  npc->setVisual(visual);
  }

void GameScript::mdl_setvisualbody(std::shared_ptr<daedalus::INpc> npcRef, std::string_view body, int bodyTexNr, int bodyTexColor, std::string_view head, int headTexNr, int teethTexNr, int armor) {
  LoadProbe::Startup::Detail::Scope detail("mdl_setvisualbody",[&]{return std::string(string_frm(body,":",bodyTexNr,":",bodyTexColor,":",head,":",headTexNr,":",teethTexNr,":",armor));});
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;

  npc->setVisualBody(headTexNr,teethTexNr,bodyTexNr,bodyTexColor,body,head);
  if(armor>0) {
    if(npc->itemCount(uint32_t(armor))==0)
      npc->addItem(uint32_t(armor),1);
    npc->useItem(uint32_t(armor),Item::NSLOT,true);
    }
  }

void GameScript::mdl_setmodelfatness(std::shared_ptr<daedalus::INpc> npcRef, float fat) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setFatness(fat);
  }

void GameScript::mdl_applyoverlaymds(std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname) {
  LoadProbe::Startup::Detail::Scope detail("mdl_applyoverlaymds",[&]{return std::string(overlayname);});
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->addOverlay(skelet,0);
    }
  }

void GameScript::mdl_applyoverlaymdstimed(std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname, int ticks) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ticks>0) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->addOverlay(skelet,uint64_t(ticks));
    }
  }

void GameScript::mdl_removeoverlaymds(std::shared_ptr<daedalus::INpc> npcRef, std::string_view overlayname) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->delOverlay(skelet);
  }
}

void GameScript::mdl_setmodelscale(std::shared_ptr<daedalus::INpc> npcRef, float x, float y, float z) {
  auto npc = findNpc(npcRef);
  if(npcRef!=nullptr)
    npc->setScale(x,y,z);
  }

void GameScript::mdl_startfaceani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view ani, float intensity, float time) {
  if(npcRef!=nullptr)
    findNpc(npcRef.get())->startFaceAnim(ani,intensity,uint64_t(time*1000.f));
  }

void GameScript::mdl_applyrandomani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view s1, std::string_view s0) {
  (void)npcRef;
  (void)s1;
  (void)s0;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomani]");
    first=false;
    }
  }

void GameScript::mdl_applyrandomanifreq(std::shared_ptr<daedalus::INpc> npcRef, std::string_view s1, float f0) {
  (void)f0;
  (void)s1;
  (void)npcRef;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomanifreq]");
    first=false;
    }
  }

void GameScript::mdl_applyrandomfaceani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view name, float timeMin, float timeMinVar, float timeMax, float timeMaxVar, float probMin) {
  (void)probMin;
  (void)timeMaxVar;
  (void)timeMax;
  (void)timeMinVar;
  (void)timeMin;
  (void)name;
  (void)npcRef;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomfaceani]");
    first=false;
    }
  }

void GameScript::wld_insertnpc(int npcInstance, std::string_view spawnpoint) {
  LoadProbe::Startup::Detail::Scope detail("wld_insertnpc",[&]{return std::string(spawnpoint);});
  if(npcInstance<=0)
    return;

  auto npc = world().addNpc(size_t(npcInstance),spawnpoint);
  if(npc!=nullptr)
    fixNpcPosition(*npc,0,150);
  }

void GameScript::wld_removenpc(int npcInstance) {
  if(auto npc = findNpcById(size_t(npcInstance)))
    world().removeNpc(*npc);
  }

void GameScript::wld_insertitem(int itemInstance, std::string_view spawnpoint) {
  if(spawnpoint.empty() || itemInstance<=0)
    return;

  world().addItem(size_t(itemInstance),spawnpoint);
  }

void GameScript::ai_processinfos(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto pl  = owner.player();
  if(pl!=nullptr && npc!=nullptr) {
    aiOutOrderId=0;
    npc->aiPush(AiQueue::aiProcessInfo(*pl));
    }
  }

void GameScript::ai_output(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view outputname) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutput(*target,outputname,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_stopprocessinfos(std::shared_ptr<daedalus::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self) {
    self->aiPush(AiQueue::aiStopProcessInfo(aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_standup(std::shared_ptr<daedalus::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStandup());
  }

void GameScript::ai_standupquick(std::shared_ptr<daedalus::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStandupQuick());
  }

void GameScript::ai_continueroutine(std::shared_ptr<daedalus::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiContinueRoutine());
  }

void GameScript::ai_stoplookat(std::shared_ptr<daedalus::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStopLookAt());
  }

void GameScript::ai_lookat(std::shared_ptr<daedalus::INpc> selfRef, std::string_view waypoint) {
  auto self = findNpc(selfRef);
  auto to  = world().findPoint(waypoint);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiLookAt(to));
  }

void GameScript::ai_lookatnpc(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiLookAtNpc(npc));
  }

void GameScript::ai_removeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiRemoveWeapon());
  }

void GameScript::ai_unreadyspell(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiRemoveWeapon());
  }

void GameScript::ai_turnaway(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiTurnAway(npc));
  }

void GameScript::ai_turntonpc(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiTurnToNpc(npc));
  }

void GameScript::ai_whirlaround(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiWhirlToNpc(npc));
  }

void GameScript::ai_outputsvm(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view name) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutputSvm(*target,name,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_outputsvm_overlay(std::shared_ptr<daedalus::INpc> selfRef, std::shared_ptr<daedalus::INpc> targetRef, std::string_view name) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutputSvmOverlay(*target,name,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_startstate(std::shared_ptr<daedalus::INpc> selfRef, int func, int state, std::string_view wp) {
  auto self = findNpc(selfRef);
  if(self!=nullptr && func>0) {
    Npc*  oth = findNpc(vm.global_other());
    Npc*  vic = findNpc(vm.global_victim());
    auto& st  = aiState(size_t(func));
    self->aiPush(AiQueue::aiStartState(st.funcIni,state,oth,vic,wp));
    }
  }

void GameScript::ai_playani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiPlayAnim(name));
  }

void GameScript::ai_setwalkmode(std::shared_ptr<daedalus::INpc> npcRef, int modeBits) {
  int32_t weaponBit = 0x80;
  auto npc = findNpc(npcRef);

  int32_t mode = modeBits & (~weaponBit);
  if(npc!=nullptr && mode>=0 && mode<=3){ //TODO: weapon flags
    npc->aiPush(AiQueue::aiSetWalkMode(WalkBit(mode)));
    }
  }

void GameScript::ai_wait(std::shared_ptr<daedalus::INpc> npcRef, float ms) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ms>0)
    npc->aiPush(AiQueue::aiWait(uint64_t(ms*1000)));
  }

void GameScript::ai_waitms(std::shared_ptr<daedalus::INpc> npcRef, int ms) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ms>0)
    npc->aiPush(AiQueue::aiWait(uint64_t(ms)));
  }

void GameScript::ai_aligntowp(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiAlignToWp());
  }

void GameScript::ai_gotowp(std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;

  auto to = world().findWayPoint(npc->position(), waypoint);
  if(to!=nullptr) {
    npc->aiPush(AiQueue::aiGoToPoint(*to));
    return;
    }

  // in vanilla 'ai_gotowp' sometimes is used incorrectly, so we need to check all other points
  to = world().findPoint(waypoint, false);
  if(to!=nullptr)
    npc->aiPush(AiQueue::aiGoToPoint(*to));
  }

void GameScript::ai_gotofp(std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);

  if(npc) {
    auto to = world().findFreePoint(*npc,waypoint);
    if(to!=nullptr)
      npc->aiPush(AiQueue::aiGoToPoint(*to));
    }
  }

void GameScript::ai_playanibs(std::shared_ptr<daedalus::INpc> npcRef, std::string_view ani, int bs) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiPlayAnimBs(ani,BodyState(bs)));
  }

void GameScript::ai_equiparmor(std::shared_ptr<daedalus::INpc> npcRef, int id) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipArmor(id));
  }

void GameScript::ai_equipbestarmor(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestArmor());
  }

int GameScript::ai_equipbestmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestMeleeWeapon());
  return 0;
  }

int GameScript::ai_equipbestrangedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestRangedWeapon());
  return 0;
  }

bool GameScript::ai_usemob(std::shared_ptr<daedalus::INpc> npcRef, std::string_view tg, int state) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUseMob(tg,state));
  return 0;
  }

void GameScript::ai_teleport(std::shared_ptr<daedalus::INpc> npcRef, std::string_view tg) {
  auto npc = findNpc(npcRef);
  auto pt  = world().findPoint(tg);
  if(npc!=nullptr && pt!=nullptr)
    npc->aiPush(AiQueue::aiTeleport(*pt));
  }

void GameScript::ai_stoppointat(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiStopPointAt());
  }

void GameScript::ai_drawweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiDrawWeapon());
  }

void GameScript::ai_readymeleeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiReadyMeleeWeapon());
  }

void GameScript::ai_readyrangedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiReadyRangedWeapon());
  }

void GameScript::ai_readyspell(std::shared_ptr<daedalus::INpc> npcRef, int spell, int mana) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && mana>0)
    npc->aiPush(AiQueue::aiReadySpell(spell,mana));
  }

void GameScript::ai_attack(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiAttack());
  }

void GameScript::ai_flee(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiFlee());
  }

void GameScript::ai_dodge(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiDodge());
  }

void GameScript::ai_unequipweapons(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUnEquipWeapons());
  }

void GameScript::ai_unequiparmor(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUnEquipArmor());
  }

void GameScript::ai_gotonpc(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> toRef) {
  auto to  = findNpc(toRef);
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiGoToNpc(to));
  }

void GameScript::ai_gotonextfp(std::shared_ptr<daedalus::INpc> npcRef, std::string_view to) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiGoToNextFp(to));
  }

void GameScript::ai_aligntofp(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiAlignToFp());
  }

void GameScript::ai_useitem(std::shared_ptr<daedalus::INpc> npcRef, int item) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiUseItem(item));
  }

void GameScript::ai_useitemtostate(std::shared_ptr<daedalus::INpc> npcRef, int item, int state) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiUseItemToState(item,state));
  }

void GameScript::ai_setnpcstostate(std::shared_ptr<daedalus::INpc> npcRef, int state, int radius) {
  auto npc = findNpc(npcRef);
  if(npc && state>0)
    npc->aiPush(AiQueue::aiSetNpcsToState(size_t(state),radius));
  }

void GameScript::ai_finishingmove(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> othRef) {
  auto oth = findNpc(othRef);
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && oth!=nullptr)
    npc->aiPush(AiQueue::aiFinishingMove(*oth));
  }

void GameScript::ai_takeitem(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && itm!=nullptr)
    npc->aiPush(AiQueue::aiTakeItem(*itm));
  }

void GameScript::ai_gotoitem(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && itm!=nullptr)
    npc->aiPush(AiQueue::aiGotoItem(*itm));
  }

void GameScript::ai_pointat(std::shared_ptr<daedalus::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);
  auto to  = world().findPoint(waypoint);
  if(npc!=nullptr && to!=nullptr)
    npc->aiPush(AiQueue::aiPointAt(*to));
  }

void GameScript::ai_pointatnpc(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef) {
  auto other = findNpc(otherRef);
  auto npc   = findNpc(npcRef);
  if(npc!=nullptr && other!=nullptr)
    npc->aiPush(AiQueue::aiPointAtNpc(*other));
  }

int GameScript::ai_printscreen(std::string_view msg, int posx, int posy, std::string_view font, int timesec) {
  auto npc = findNpc(vm.global_self());
  if(npc==nullptr)
    npc = owner.player();
  if(npc==nullptr) {
    Gothic::inst().onPrintScreen(msg,posx,posy,timesec,Resources::font(font,Resources::FontType::Normal,1.0));
    return 0;
    }
  npc->aiPush(AiQueue::aiPrintScreen(timesec,font,posx,posy,msg));
  return 0;
  }

int GameScript::mob_hasitems(std::string_view tag, int item) {
  return int(world().hasItems(tag,uint32_t(item)));
  }

void GameScript::ta_min(std::shared_ptr<daedalus::INpc> npcRef, int start_h, int start_m, int stop_h, int stop_m, int action, std::string_view waypoint) {
  LoadProbe::Startup::Detail::Scope detail("ta_min",[&]{return std::string(waypoint);});
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->addRoutine(gtime(start_h,start_m),gtime(stop_h,stop_m),uint32_t(action),waypoint);
  }

void GameScript::log_createtopic(std::string_view topicName, int section) {
  if(section==QuestLog::Mission || section==QuestLog::Note)
    quests.add(topicName,QuestLog::Section(section));
  }

void GameScript::log_settopicstatus(std::string_view topicName, int status) {
  if(status==int32_t(QuestLog::Status::Running) ||
     status==int32_t(QuestLog::Status::Success) ||
     status==int32_t(QuestLog::Status::Failed ) ||
     status==int32_t(QuestLog::Status::Obsolete))
    quests.setStatus(topicName,QuestLog::Status(status));
  }

void GameScript::log_addentry(std::string_view topicName, std::string_view entry) {
  quests.addEntry(topicName,entry);
  }

void GameScript::equipitem(std::shared_ptr<daedalus::INpc> npcRef, int cls) {
  LoadProbe::Startup::Detail::Scope detail("equipitem",[&]{
    const auto* symbol=vm.find_symbol_by_index(uint32_t(cls));
    return symbol!=nullptr ? symbol->name() : std::to_string(cls);
    });
  auto self = findNpc(npcRef);
  if(self!=nullptr) {
    if(self->itemCount(uint32_t(cls))==0)
      self->addItem(uint32_t(cls),1);
    self->useItem(uint32_t(cls),Item::NSLOT,true);
    }
  }

void GameScript::createinvitem(std::shared_ptr<daedalus::INpc> npcRef, int itemInstance) {
  LoadProbe::Startup::Detail::Scope detail("createinvitem",[&]{
    const auto* symbol=vm.find_symbol_by_index(uint32_t(itemInstance));
    return symbol!=nullptr ? symbol->name() : std::to_string(itemInstance);
    });
  auto self = findNpc(npcRef);
  if(self!=nullptr) {
    Item* itm = self->addItem(uint32_t(itemInstance),1);
    storeItem(itm);
    }
  }

void GameScript::createinvitems(std::shared_ptr<daedalus::INpc> npcRef, int itemInstance, int amount) {
  LoadProbe::Startup::Detail::Scope detail("createinvitems",[&]{
    const auto* symbol=vm.find_symbol_by_index(uint32_t(itemInstance));
    return (symbol!=nullptr ? symbol->name() : std::to_string(itemInstance))+":"+std::to_string(amount);
    });
  auto self = findNpc(npcRef);
  if(self!=nullptr && amount>0) {
    Item* itm = self->addItem(uint32_t(itemInstance),size_t(amount));
    storeItem(itm);
    }
  }

void GameScript::perc_setrange(int perc, int dist) {
  if(perc<0 || perc>=PERC_Count)
    return;
  perceptionRanges.range[perc] = dist;
  }

int GameScript::hlp_getinstanceid(std::shared_ptr<daedalus::DaedalusInstance> instance) {
  // Log::d("hlp_getinstanceid: name \"",handle.name,"\" not found");
  return instance == nullptr ? -1 : int(instance->symbol_index());
  }

int GameScript::hlp_random(int bound) {
  uint32_t   mod  = uint32_t(std::max(1,bound));
  const Npc* self = nullptr;
  if(!isolatedRandom.empty() && vm.global_self()!=nullptr)
    self = findNpc(vm.global_self());
  return int32_t(rand(mod,self));
  }

bool GameScript::hlp_isvalidnpc(std::shared_ptr<daedalus::INpc> npcRef) {
  auto self = findNpc(npcRef);
  return self != nullptr;
  }

bool GameScript::hlp_isitem(std::shared_ptr<daedalus::IItem> itemRef, int instanceSymbol) {
  auto item = findItem(itemRef.get());
  if(item!=nullptr){
    auto& v = item->handle();
    return int(v.symbol_index()) == instanceSymbol;
    } else {
      return false;
    }
  }

bool GameScript::hlp_isvaliditem(std::shared_ptr<daedalus::IItem> itemRef) {
  auto item = findItem(itemRef.get());
  return item!=nullptr;
  }

std::shared_ptr<daedalus::INpc> GameScript::hlp_getnpc(int instanceSymbol) {
  auto npc = findNpcById(uint32_t(instanceSymbol));
  if(npc != nullptr)
    return npc->handlePtr();
  else
    return nullptr;
  }

void GameScript::info_addchoice(int infoInstance, std::string_view text, int func) {
  auto info = findInfo(uint32_t(infoInstance));
  if(info==nullptr)
    return;
  daedalus::IInfoChoice choice {};
  choice.text     = text;
  choice.function = func;
  info->add_choice(choice);
  }

void GameScript::info_clearchoices(int infoInstance) {
  auto info = findInfo(uint32_t(infoInstance));
  if(info==nullptr)
    return;
  info->choices.clear();
  }

bool GameScript::infomanager_hasfinished() {
  return !owner.isInDialog();
  }

void GameScript::snd_play(std::string_view fileS) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  std::string file {fileS};
  for(auto& c:file)
    c = char(std::toupper(c));
  Gothic::inst().emitGlobalSound(file);
  }

void GameScript::snd_play3d(std::shared_ptr<daedalus::INpc> npcRef, std::string_view fileS) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  std::string file {fileS};
  Npc*        npc  = findNpc(npcRef);
  if(npc==nullptr)
    return;
  for(auto& c:file)
    c = char(std::toupper(c));
  auto sfx = ::Sound(*owner.world(),::Sound::T_3D,file,npc->centerPosition(),0.f,false);
  sfx.play();
  }

void GameScript::exitsession() {
  if(auto& take = Gothic::inst().takeGameEnd; take && take("finale"))
    return;
  owner.exitSession();
  }

void GameScript::sort(std::vector<GameScript::DlgChoice> &dlg) {
  std::sort(dlg.begin(),dlg.end(),[](const GameScript::DlgChoice& l,const GameScript::DlgChoice& r){
    return std::tie(l.sort,l.scriptFn)<std::tie(r.sort,r.scriptFn); // small hack with scriptfn to reproduce behavior of original game
    });
  }

void GameScript::setNpcInfoKnown(const daedalus::INpc& npc, const daedalus::IInfo& info) {
  auto id = std::make_pair(vm.find_symbol_by_instance(npc)->index(),vm.find_symbol_by_instance(info)->index());
  dlgKnownInfos.insert(id);
  }

bool GameScript::doesNpcKnowInfo(const daedalus::INpc& npc, size_t infoInstance) const {
  auto id = std::make_pair(vm.find_symbol_by_instance(npc)->index(),infoInstance);
  return dlgKnownInfos.find(id)!=dlgKnownInfos.end();
  }
