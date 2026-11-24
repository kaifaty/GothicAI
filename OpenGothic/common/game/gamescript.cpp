#include "gamescript.h"
#include "gamescript_programs.h"
#include "world/npcactivityprobe.h"

#include <Tempest/Log>
#include <Tempest/SoundEffect>

#include <cctype>
#include <optional>
#include <sstream>
#include <tuple>
#include <type_traits>

#include "game/definitions/basescripts.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/spelldefinitions.h"
#include "game/serialize.h"
#include "game/definitions/scriptvars.h"
#include "game/herodeed.h"
#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/triggers/abstracttrigger.h"
#include "graphics/visualfx.h"
#include "utils/fileutil.h"
#include "commandline.h"
#include "gothic.h"
#include "settings.h"
#include "loadprobe.h"
#include "outputunits.h"
#include "programdata.h"
#include "navdemos.h"

using namespace Tempest;

template <typename T>
struct ScopeVar final {
  ScopeVar(daedalus::DaedalusSymbol& sym, const std::shared_ptr<T>& h) : prev(sym.get_instance()), sym(sym) {
    sym.set_instance(std::static_pointer_cast<daedalus::INpc>(h));
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

namespace {
struct BennetLookupAudit final {
  explicit BennetLookupAudit(const std::function<void(bool)>& scope) : scope(scope) {
    if(scope)
      scope(true);
    }
  ~BennetLookupAudit() {
    if(scope)
      scope(false);
    }
  const std::function<void(bool)>& scope;
  };
}


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

GameScript::GameScript(GameSession &owner)
    :owner(owner), vm(createVm(Gothic::inst(),itemDefinitions,routineDefinitions,npcDefinitions,placementDefinitions,
                              activityDefinitions,quietSound,nativeRoutineProgram)) {
  if (vm.global_self() == nullptr || vm.global_other() == nullptr || vm.global_item() == nullptr ||
      vm.global_victim() == nullptr || vm.global_hero() == nullptr)
    throw std::runtime_error("Cannot find script symbol SELF, OTHER, ITEM, VICTIM, or HERO! Cannot proceed!");

  vmLang = Settings::getI("GAME", "language");
  Gothic::inst().setupCommonScriptClasses(vm);
  Gothic::inst().setupVmCommonApi(vm);
  aiDefaultPipe.reset(new GlobalOutput(*this));
  loadNativeAiStates();
  initCommon();
  loadNativeNpcDeath();
  loadNativeTradeInventory();
  loadNativeDeathTrophies();
  loadNativeNpcGlobals();
  loadNativeMonsterAttitudes();
  loadNativeWorldEntry();
  loadNativeRoutineExchange();
  bindPreparedInventory();
  loadNativeRoutines();
  loadNativeRoomReaction();
  loadNativeHumanPerceptions();
  loadNativeEquipment();
  loadNativeTalkReaction();
  loadNativeAmbientInfos();
  loadNativeTrainingHelpers();
  initBennetOre();
  initSettings();
  Settings::inst().onSettingsChanged.bind(this,&GameScript::initSettings);
  }

GameScript::~GameScript() {
  Settings::inst().onSettingsChanged.ubind(this,&GameScript::initSettings);
  }

namespace {

using storygraph::BoundNativeCall;
using storygraph::NativeCall;
using storygraph::NativeValue;

template<class T>
constexpr NativeValue::Type externalType() {
  if constexpr(std::is_integral_v<T>) {
    return NativeValue::Int;
    } else if constexpr(std::is_same_v<T,float>) {
    return NativeValue::Float;
    } else if constexpr(std::is_same_v<T,std::string_view>) {
    return NativeValue::Str;
    } else {
    static_assert(daedalus::DaedalusInstancePointer<T>);
    return NativeValue::Inst;
    }
  }

template<class T>
T externalArgument(const NativeValue& value, const BoundNativeCall::ResolveActor& resolve) {
  if constexpr(std::is_integral_v<T>) {
    return static_cast<T>(value.i);
    } else if constexpr(std::is_same_v<T,float>) {
    return value.f;
    } else if constexpr(std::is_same_v<T,std::string_view>) {
    return value.s;
    } else {
    using Instance = typename T::element_type;
    auto instance = resolve(value.inst);
    if(value.inst!=0 && instance==nullptr)
      throw std::runtime_error("Unbound instance argument");
    if constexpr(!std::is_same_v<Instance,daedalus::DaedalusInstance>) {
      if(instance!=nullptr && (instance->instance_type()==nullptr || *instance->instance_type()!=typeid(Instance)))
        throw std::runtime_error("Instance argument has the wrong type");
      }
    return std::static_pointer_cast<Instance>(std::move(instance));
    }
  }

}

template<class R, class... P>
std::function<R(P...)> GameScript::externalCallback(const std::string& name, R(GameScript::*function)(P...)) {
  const size_t floorId = floorDefer.registerExternal(name,StartupFloor::classify(name));
  return [this,function,floorId](P... values) {
    if(floorDefer.active())
      floorBarrier(floorId,values...);
    return (this->*function)(values...);
    };
  }

template<class R, class... P>
void GameScript::bindExternal(const std::string& name, R(GameScript::*function)(P...)) {
  vm.register_external(name,externalCallback(name,function));
  }

template<class R, class... P>
void GameScript::bindNativeExternal(const std::string& name, R(GameScript::*function)(P...)) {
  const auto callback = externalCallback(name,function);
  vm.register_external(name,callback);
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr)
    return; // A reference program may omit this external.

  storygraph::NativeExternal external;
  external.signature.parameters = {externalType<P>()...};
  if constexpr(!std::is_void_v<R>)
    external.signature.result = externalType<R>();
  external.call = [callback](std::span<const NativeValue> args, const BoundNativeCall::ResolveActor& resolve,
                             const BoundNativeCall::BindActor& bind) -> NativeCall {
    const auto parameters = [&]<size_t... I>(std::index_sequence<I...>) {
      return std::tuple<P...>{externalArgument<P>(args[I],resolve)...};
      }(std::index_sequence_for<P...>{});
    if constexpr(std::is_void_v<R>) {
      std::apply(callback,parameters);
      return {};
      } else if constexpr(daedalus::DaedalusInstancePointer<R>) {
      return {NativeValue::instance(bind(std::apply(callback,parameters))),{}};
      } else {
      return {NativeValue::integer(int32_t(std::apply(callback,parameters))),{}};
      }
    };
  nativeExternals.insert_or_assign(symbol->name(),std::move(external));
  }

void GameScript::initCommon() {
  bindExternal("hlp_random",                     &GameScript::hlp_random);
  bindNativeExternal("hlp_isvalidnpc",                 &GameScript::hlp_isvalidnpc);
  bindNativeExternal("hlp_isvaliditem",                &GameScript::hlp_isvaliditem);
  bindNativeExternal("hlp_isitem",                     &GameScript::hlp_isitem);
  bindNativeExternal("hlp_getnpc",                     &GameScript::hlp_getnpc);
  bindNativeExternal("hlp_getinstanceid",              &GameScript::hlp_getinstanceid);

  bindNativeExternal("wld_insertnpc",                  &GameScript::wld_insertnpc);
  bindNativeExternal("wld_removenpc",                  &GameScript::wld_removenpc);
  bindNativeExternal("wld_insertitem",                 &GameScript::wld_insertitem);
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

  bindNativeExternal("mdl_setvisual",                  &GameScript::mdl_setvisual);
  bindNativeExternal("mdl_setvisualbody",              &GameScript::mdl_setvisualbody);
  bindNativeExternal("mdl_setmodelfatness",            &GameScript::mdl_setmodelfatness);
  bindNativeExternal("mdl_applyoverlaymds",            &GameScript::mdl_applyoverlaymds);
  bindExternal("mdl_applyoverlaymdstimed",       &GameScript::mdl_applyoverlaymdstimed);
  bindExternal("mdl_removeoverlaymds",           &GameScript::mdl_removeoverlaymds);
  bindNativeExternal("mdl_setmodelscale",              &GameScript::mdl_setmodelscale);
  bindExternal("mdl_startfaceani",               &GameScript::mdl_startfaceani);
  bindExternal("mdl_applyrandomani",             &GameScript::mdl_applyrandomani);
  bindExternal("mdl_applyrandomanifreq",         &GameScript::mdl_applyrandomanifreq);
  bindExternal("mdl_applyrandomfaceani",         &GameScript::mdl_applyrandomfaceani);

  bindNativeExternal("npc_settofightmode",             &GameScript::npc_settofightmode);
  bindNativeExternal("npc_settofistmode",              &GameScript::npc_settofistmode);
  bindExternal("npc_isinstate",                  &GameScript::npc_isinstate);
  bindNativeExternal("npc_isinroutine",                &GameScript::npc_isinroutine);
  bindExternal("npc_wasinstate",                 &GameScript::npc_wasinstate);
  bindExternal("npc_getdisttowp",                &GameScript::npc_getdisttowp);
  bindNativeExternal("npc_exchangeroutine",            &GameScript::npc_exchangeroutine);
  bindNativeExternal("npc_isdead",                     &GameScript::npc_isdead);
  bindExternal("npc_knowsinfo",                  &GameScript::npc_knowsinfo);
  bindNativeExternal("npc_settalentskill",             &GameScript::npc_settalentskill);
  bindNativeExternal("npc_gettalentskill",             &GameScript::npc_gettalentskill);
  bindExternal("npc_settalentvalue",             &GameScript::npc_settalentvalue);
  bindExternal("npc_gettalentvalue",             &GameScript::npc_gettalentvalue);
  bindExternal("npc_setrefusetalk",              &GameScript::npc_setrefusetalk);
  bindExternal("npc_refusetalk",                 &GameScript::npc_refusetalk);
  bindNativeExternal("npc_hasitems",                   &GameScript::npc_hasitems);
  bindNativeExternal("npc_hasspell",                   &GameScript::npc_hasspell);
  bindNativeExternal("npc_getinvitem",                 &GameScript::npc_getinvitem);
  bindNativeExternal("npc_getinvitembyslot",           &GameScript::npc_getinvitembyslot);
  bindNativeExternal("npc_removeinvitem",              &GameScript::npc_removeinvitem);
  bindNativeExternal("npc_removeinvitems",             &GameScript::npc_removeinvitems);
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
  bindNativeExternal("npc_getequippedmeleeweapon",     &GameScript::npc_getequippedmeleeweapon);
  bindNativeExternal("npc_getequippedrangedweapon",    &GameScript::npc_getequippedrangedweapon);
  bindNativeExternal("npc_getequippedarmor",           &GameScript::npc_getequippedarmor);
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
  bindNativeExternal("npc_clearinventory",             &GameScript::npc_clearinventory);
  bindExternal("npc_getattitude",                &GameScript::npc_getattitude);
  bindExternal("npc_getpermattitude",            &GameScript::npc_getpermattitude);
  bindNativeExternal("npc_setattitude",                &GameScript::npc_setattitude);
  bindNativeExternal("npc_settempattitude",            &GameScript::npc_settempattitude);
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
  bindNativeExternal("ai_standup",                     &GameScript::ai_standup);
  bindNativeExternal("ai_standupquick",                &GameScript::ai_standupquick);
  bindNativeExternal("ai_continueroutine",             &GameScript::ai_continueroutine);
  bindExternal("ai_stoplookat",                  &GameScript::ai_stoplookat);
  bindExternal("ai_lookat",                      &GameScript::ai_lookat);
  bindExternal("ai_lookatnpc",                   &GameScript::ai_lookatnpc);
  bindExternal("ai_removeweapon",                &GameScript::ai_removeweapon);
  bindExternal("ai_unreadyspell",                &GameScript::ai_unreadyspell);
  bindExternal("ai_turnaway",                    &GameScript::ai_turnaway);
  bindExternal("ai_turntonpc",                   &GameScript::ai_turntonpc);
  bindExternal("ai_whirlaround",                 &GameScript::ai_whirlaround);
  bindNativeExternal("ai_outputsvm",                   &GameScript::ai_outputsvm);
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

  bindNativeExternal("ta_min",                         &GameScript::ta_min);

  bindExternal("log_createtopic",                &GameScript::log_createtopic);
  bindExternal("log_settopicstatus",             &GameScript::log_settopicstatus);
  bindExternal("log_addentry",                   &GameScript::log_addentry);

  bindNativeExternal("equipitem",                      &GameScript::equipitem);
  bindNativeExternal("createinvitem",                  &GameScript::createinvitem);
  bindNativeExternal("createinvitems",                 &GameScript::createinvitems);

  bindExternal("perc_setrange",                  &GameScript::perc_setrange);

  bindExternal("info_addchoice",                 &GameScript::info_addchoice);
  bindExternal("info_clearchoices",              &GameScript::info_clearchoices);
  bindExternal("infomanager_hasfinished",        &GameScript::infomanager_hasfinished);

  bindNativeExternal("snd_play",                       &GameScript::snd_play);
  bindExternal("snd_play3d",                     &GameScript::snd_play3d);

  bindExternal("game_initgerman",                &GameScript::game_initgerman);
  bindExternal("game_initenglish",               &GameScript::game_initenglish);

  bindExternal("exitsession",                    &GameScript::exitsession);

  // vm.validateExternals();

  const auto gameplay = nativeRoutineProgram && NpcDefinitionRuntime::enabled() ?
                        GameplayDefinitions::load(baseProgramIndex().dir.parent_path()) : nullptr;
  spells = gameplay ? std::make_unique<SpellDefinitions>(*gameplay) : std::make_unique<SpellDefinitions>(vm);
  svm    = gameplay ? std::make_unique<SvmDefinitions>(gameplay)    : std::make_unique<SvmDefinitions>(vm);

  const auto focus = [&](std::string_view name) -> FocusRecord {
    return gameplay ? gameplay->focus(name) : findFocus(name);
    };
  cFocusNorm           = focus("FOCUS_NORMAL");
  cFocusMelee          = focus("FOCUS_MELEE");
  cFocusRange          = focus("FOCUS_RANGED");
  cFocusMage           = focus("FOCUS_MAGIC");

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
    goldTxt = createItemForQuery(itMi_Gold->index())->name;
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

  if(gameplay)
    cGuildVal = std::make_shared<GuildRecord>(gameplay->guild());
  else if(auto* id = vm.find_symbol_by_name("Gil_Values"))
    cGuildVal = vm.init_instance<daedalus::IGuildValues>(id);
  if(cGuildVal){
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
  auto lang = Settings::getI("GAME", "language");
  if(vmLang!=lang) {
    vmLang = lang;
    //vm     = createVm(Gothic::inst());
    initDialogs();
    }
  }

void GameScript::initDialogs() {
  loadDialogOU();

  storyDialogsById.clear();
  storyDialogs.clear();
  dialogsInfo.clear();
  const bool native = nativeRoutineProgram && activityEvents && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  if(native && readDialogInfos())
    return;
  vm.enumerate_instances_by_class_name("C_INFO", [this](daedalus::DaedalusSymbol& sym){
    dialogsInfo.push_back(vm.init_instance<daedalus::IInfo>(&sym));
    rememberStoryDialog(sym.name(),*dialogsInfo.back());
    });
  }

void GameScript::loadDialogOU() {
  // Based on https://github.com/auronen/Gothic-2-localization mod,
  // at least in G2 .BIN should have a priority
  const std::string prefix = std::string(Gothic::inst().defaultOutputUnits());
  const std::array<std::string,2> names = {prefix + ".BIN", prefix + ".DAT"};

  for(auto& OU:names) {
    if(loadBaseOutputUnits(OU,vmLang,dialogs))
      return;
    }
  Log::e("none of Zen-files for OU could be loaded");
  }

void GameScript::initializeInstanceNpc(const std::shared_ptr<daedalus::INpc>& npc, size_t instance, bool useRecords) {
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
  if(auto* like = own.likeOf(vm,sym->index()))
    mine.emplace(*like,npc);

  initializeNpcDefinition(npc,sym,useRecords);

  if(npc->daily_routine!=0) {
    ScopeVar self(*vm.global_self(), npc);
    auto* daily_routine = vm.find_symbol_by_index(uint32_t(npc->daily_routine));
    if(daily_routine != nullptr) {
      LoadProbe::Startup::Detail::Scope routine("daily_routine",[&]{return daily_routine->name();});
      initializeRoutine(npc,daily_routine);
      }
    }
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
  std::vector<ScriptVars::Row> rows;
  for(uint32_t id=0; id<vm.symbols().size(); ++id) {
    if(isBennetProgressSymbol(id))
      continue;
    auto row = stateRow(*vm.find_symbol_by_index(id));
    if(!std::holds_alternative<std::monostate>(row.value))
      rows.push_back(std::move(row));
    }
  fout.write(uint32_t(rows.size()));
  for(const auto& row : rows)
    ScriptVars::writeRow(fout,row);
  }

void GameScript::loadVar(Serialize &fin) {
  uint32_t count = 0;
  fin.read(count);
  for(uint32_t i=0; i<count; ++i) {
    const auto row = ScriptVars::readRow(fin);
    if(std::holds_alternative<std::monostate>(row.value))
      continue;

    auto* symbol = findSymbol(row.name);
    const auto loadValues = [&](const auto& values, auto setter) {
      if(symbol==nullptr || symbol->is_member() || symbol->is_const())
        return;
      for(size_t index=0; index<values.size(); ++index)
        (symbol->*setter)(values[index],uint16_t(index),nullptr);
      };
    if(const auto* values = std::get_if<std::vector<int32_t>>(&row.value)) {
      loadValues(*values,&daedalus::DaedalusSymbol::set_int);
      }
    else if(const auto* values = std::get_if<std::vector<float>>(&row.value)) {
      loadValues(*values,&daedalus::DaedalusSymbol::set_float);
      }
    else if(const auto* values = std::get_if<std::vector<std::string>>(&row.value)) {
      loadValues(*values,&daedalus::DaedalusSymbol::set_string);
      }
    else if(const auto* ref = std::get_if<ScriptVars::Reference>(&row.value); ref!=nullptr && symbol!=nullptr) {
      switch(ref->kind) {
        case ScriptVars::ReferenceKind::Npc: {
          const auto* npc = world().npcById(ref->object);
          symbol->set_instance(npc ? std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) : nullptr);
          break;
          }
        case ScriptVars::ReferenceKind::WorldItem: {
          auto* item = world().itmById(ref->object);
          symbol->set_instance(item ? std::static_pointer_cast<daedalus::IItem>(item->handlePtr()) : nullptr);
          break;
          }
        case ScriptVars::ReferenceKind::InventoryItem: {
          const auto* npc = world().npcById(ref->object);
          auto* item = npc ? npc->inventory().itemByIndex(ref->item) : nullptr;
          symbol->set_instance(item ? std::static_pointer_cast<daedalus::IItem>(item->handlePtr()) : nullptr);
          break;
          }
        default:
          break;
        }
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
    initPerceptions();
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

void GameScript::saveSym(Serialize &fout, daedalus::DaedalusSymbol& symbol) {
  ScriptVars::writeRow(fout,stateRow(symbol));
  }

ScriptVars::Row GameScript::stateRow(daedalus::DaedalusSymbol& symbol) {
  using Type = daedalus::DaedalusDataType;
  using Kind = ScriptVars::ReferenceKind;
  ScriptVars::Row row;
  if(symbol.type()==Type::INSTANCE) {
    if(symbol.is_instance_of<daedalus::INpc>()) {
      const auto* record = static_cast<const daedalus::INpc*>(symbol.get_instance().get());
      const auto* npc = static_cast<const Npc*>(record->user_ptr);
      row = {symbol.name(),ScriptVars::Reference{Kind::Npc,world().npcId(npc)}};
      }
    else if(symbol.is_instance_of<daedalus::IItem>()) {
      const auto* item = static_cast<const daedalus::IItem*>(symbol.get_instance().get());
      const auto id = world().itmId(static_cast<const ItemRecord*>(item));
      ScriptVars::Reference ref{};
      if(id!=uint32_t(-1)) {
        ref = {Kind::WorldItem,id};
        } else {
        for(uint32_t ownerId=0; ownerId<world().npcCount(); ++ownerId) {
          if(const auto* npc=world().npcById(ownerId)) {
            const auto& inv = npc->inventory();
            const auto index = inv.indexOf(static_cast<const Item*>(item->user_ptr));
            const auto* owned = inv.itemByIndex(index);
            if(owned==nullptr || &owned->handle()!=static_cast<const ItemRecord*>(item))
              continue;
            ref = {Kind::InventoryItem,ownerId,index};
            break;
            }
          }
        }
      if(ref.kind!=Kind::None)
        row = {symbol.name(),ref};
      }
    }
  else if(symbol.count()>0 && !symbol.is_member() && !symbol.is_const()) {
    const auto values = [&](auto getter) {
      using Value = std::decay_t<decltype((symbol.*getter)(uint16_t(0),nullptr))>;
      std::vector<Value> result;
      result.reserve(symbol.count());
      for(uint32_t index=0; index<symbol.count(); ++index)
        result.push_back((symbol.*getter)(uint16_t(index),nullptr));
      return result;
      };
    switch(symbol.type()) {
      case Type::INT:    row = {symbol.name(),values(&daedalus::DaedalusSymbol::get_int)}; break;
      case Type::FLOAT:  row = {symbol.name(),values(&daedalus::DaedalusSymbol::get_float)}; break;
      case Type::STRING: row = {symbol.name(),values(&daedalus::DaedalusSymbol::get_string)}; break;
      default: break;
      }
    }
  return row;
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

// Not findNpc: a script may pass a stale instance, which the external itself rejects.
void GameScript::floorArg(Npc** args, size_t& count, const std::shared_ptr<daedalus::INpc>& v) {
  if(v!=nullptr && v->user_ptr!=nullptr)
    args[count++] = reinterpret_cast<Npc*>(v->user_ptr);
  }

void GameScript::floorArg(Npc** args, size_t& count, const std::shared_ptr<daedalus::DaedalusInstance>& v) {
  auto h = dynamic_cast<daedalus::INpc*>(v.get());
  if(h!=nullptr && h->user_ptr!=nullptr)
    args[count++] = reinterpret_cast<Npc*>(h->user_ptr);
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
    s->set_instance(std::static_pointer_cast<daedalus::IItem>(itm->handlePtr()));
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
  if(auto* slot = storySlot(name))
    return slot->index();
  if(const auto* entry = own.find(name))
    return entry->index;
  auto sym = vm.find_symbol_by_name(name);
  return sym == nullptr ? size_t(-1) : sym->index();
  }

size_t GameScript::symbolsCount() const {
  return vm.symbols().size();
  }

const SpellRecord& GameScript::spellDesc(int32_t splId) {
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

std::vector<GameScript::DlgChoice> GameScript::dialogChoices(std::shared_ptr<NpcRecord> player,
                                                             std::shared_ptr<NpcRecord> hnpc,
                                                             const std::vector<uint32_t>& except,
                                                             bool includeImp) {
  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), player);
  std::vector<daedalus::IInfo*> hDialog;
  for(auto& info : dialogsInfo) {
    if(info->npc==static_cast<int>(static_cast<const daedalus::INpc&>(*hnpc).symbol_index())) {
      hDialog.push_back(info.get());
      }
    }

  std::vector<DlgChoice> choice;
  for(int important=includeImp ? 1 : 0;important>=0;--important){
    for(auto& i:hDialog) {
      const daedalus::IInfo& info = *i;
      if(info.important!=important)
        continue;
      const auto* bennet = bennetOreDialog(info);
      bool npcKnowsInfo = false;
      if(bennet) {
        BennetLookupAudit audit(bennetLookupScope);
        npcKnowsInfo = dlgKnownInfos.contains({static_cast<const daedalus::INpc&>(*player).symbol_index(),bennet->infoId});
        } else {
        npcKnowsInfo = doesNpcKnowInfo(*player,info.symbol_index());
        }
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
      if(bennet) {
        auto& overlay = Gothic::inst().dialogScriptFn;
        int32_t result = 0;
        if(overlay && overlay(bennet->conditionSymbol->index(),result)) {
          valid = result!=0;
          } else {
          BennetLookupAudit audit(bennetLookupScope);
          auto* hero = findNpc(player.get());
          auto* npc = findNpc(hnpc.get());
          valid = hero!=nullptr && npc!=nullptr && bennetOffer(*bennet,*hero,*npc);
          }
        } else if(info.condition) {
        valid = dialogFunction(uint32_t(info.condition))!=0;
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
      Gothic::inst().dialogExtraFn(static_cast<const daedalus::INpc&>(*hnpc),important!=0,choice);
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

  if(const auto* bennet = bennetOreDialog(info); bennet!=nullptr && dlg.scriptFn==uint32_t(info.information)) {
    if(npc.instanceSymbol()!=size_t(info.npc) || &player==&npc)
      return;
    const std::pair<size_t,size_t> known = {static_cast<const daedalus::INpc&>(pl).symbol_index(),bennet->infoId};
    bool newlyKnown = false;
    {
      BennetLookupAudit audit(bennetLookupScope);
      newlyKnown = dlgKnownInfos.insert(known).second;
      }
    auto& overlay = Gothic::inst().dialogScriptFn;
    int32_t result = 0;
    if(!overlay || !overlay(bennet->answerSymbol->index(),result)) {
      BennetLookupAudit audit(bennetLookupScope);
      if(!bennetExecute(*bennet,player,npc)) {
        if(newlyKnown)
          dlgKnownInfos.erase(known);
        return;
        }
      }
    Gothic::inst().onDialogChosen(dlg,player,npc);
    return;
    }

  if(info.information==int(dlg.scriptFn)) {
    setNpcInfoKnown(pl,info);
    } else {
    for(size_t i=0;i<info.choices.size();){
      if(info.choices[i].function==int(dlg.scriptFn))
        info.choices.erase(info.choices.begin()+int(i)); else
        ++i;
      }
    }
  const auto function = storyProgramName(dlg.scriptFn);
  std::shared_ptr<HeroDeed> pickpocket;
  if(function.ends_with("_DOIT") &&
     (function.find("PICKPOCKET")!=std::string::npos || function=="DIA_CASSIA_PICKME_DOIT")) {
    pickpocket = HeroDeed::capture(player,"pickpocket",&npc);
    if(pickpocket!=nullptr) {
      pickpocket->inventoryBefore = HeroDeed::inventory(player);
      pickpocket->pickedPocketBefore = HeroDeed::aivar(npc,"AIV_PLAYERHASPICKEDMYPOCKET");
      }
    }
  dialogFunction(dlg.scriptFn);
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

void GameScript::printCannotUseError(Npc& npc, int32_t atr, int32_t nValue) {
  auto id = vm.find_symbol_by_name("G_CanNotUse");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroEquipDenial(npc.isPlayer(),atr,nValue);
  vm.call_function<void>(id, npc.isPlayer(), atr, nValue);
  }

void GameScript::printCannotCastError(Npc &npc, int32_t plM, int32_t itM) {
  auto id = vm.find_symbol_by_name("G_CanNotCast");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroCastDenial(npc.isPlayer(),itM,plM);
  vm.call_function<void>(id, npc.isPlayer(), itM, plM);
  }

void GameScript::printCannotBuyError(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_trade_not_enough_gold");
  if(id==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_TRADE_NOT_ENOUGH_GOLD");
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingItem(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_item");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_MISSINGITEM","$MISSINGITEM",true);
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKey(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_KEYMISSING","$KEYMISSING",true);
  vm.call_function<void>(id);
  }

void GameScript::printMobAnotherIsUsing(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_another_is_using");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_ANOTHERUSER",{},true);
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKeyOrLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key_or_lockpick");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_PICKLOCK_OR_KEYMISSING","$PICKLOCKORKEYMISSING",true);
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_lockpick");
  if(id==nullptr) {
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroMissingLockpick();
  vm.call_function<void>(id);
  }

void GameScript::printMobTooFar(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_too_far_away");
  if(id==nullptr) {
    owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroFeedback("PRINT_TOOFAR_AWAY",{},true);
  vm.call_function<void>(id);
  }

void GameScript::invokeState(const std::shared_ptr<NpcRecord>& hnpc, const std::shared_ptr<NpcRecord>& oth, const char *name) {
  auto id = vm.find_symbol_by_name(name);
  if(id==nullptr)
    return;

  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), oth);
  auto* npc = findNpc(hnpc.get());
  NpcActivityProbe::Phase phase(npc,id->name());
  if(npc!=nullptr && usesNativeQuietSound(ScriptFn(id->index()))) {
    phase.fromRecord();
    runQuietSoundReaction(*npc);
    }
  else if(npc==nullptr || !(invokeNativeTalkPerception(*id,phase) || invokeNativeRoutine(*npc,*id,phase) ||
                            invokeNativeMonsterPerception(*id,phase)))
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
  NpcActivityProbe::Phase phase(npc,sym ? std::string_view(sym->name()) : std::string_view{});
  int& ret = phase.result;
  if(sym!=nullptr && sym->name().starts_with("RTN_")) {
    ret = initializeRoutine(npc!=nullptr ? std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) : nullptr,sym);
    }
  else if(sym!=nullptr && npc!=nullptr && (invokeActivity(*npc,sym->index(),phase) || invokeNativeRoutine(*npc,*sym,phase))) {
    // The routine phase ran in the engine.
    }
  else if(npc!=nullptr && usesNativeQuietSound(fn)) {
    phase.fromRecord();
    runQuietSoundReaction(*npc);
    }
  else if(npc!=nullptr && ((sym!=nullptr && (invokeNativeRoomPerception(*sym,phase) ||
                                            invokeNativeMonsterPerception(*sym,phase) ||
                                            invokeNativeRoutineReaction(*npc,*sym,phase) ||
                                            invokeNativeTalkPerception(*sym,phase) ||
                                            invokeNativeStatusPerception(*npc,*sym,phase))) ||
                           invokeNativeHumanPerception(fn,phase))) {
    // The perception ran in the engine.
    }
  else if(sym!=nullptr && sym->rtype() == daedalus::DaedalusDataType::INT) {
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
  const bool nativeItems = nativeRoutineProgram && activityEvents &&
                           ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
  const auto armorAndSkillEffects = [&] {
    return applyArmorSetEffect(*npc,functionSymbol->index()) || applyFightSkillItemEffect(*npc,functionSymbol->index());
    };
  // For NPCs these callbacks only test actor identity or change talent. The
  // outer equip external still handles the pending floor fix and its neighbours.
  if(nativeItems && floorDefer.active() && !npc->isPlayer() && armorAndSkillEffects())
    return;
  if(nativeItems && !floorDefer.active()) {
    const auto amount = [&](const ItemDefinitionRuntime::RestoreChange& change) {
      const auto* symbol = std::get_if<uint32_t>(&change.amount);
      return symbol ? vm.find_symbol_by_index(*symbol)->get_int() : std::get<int32_t>(change.amount);
      };
    const auto it = itemDefinitions.restoreEffects.find(functionSymbol->index());
    if(it!=itemDefinitions.restoreEffects.end()) {
      const auto actor = std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
      const auto apply = [&](const ItemDefinitionRuntime::RestoreChange& change) {
        npc_changeattribute(actor,change.attribute,amount(change));
        };
      apply(it->second.first);
      if(it->second.second)
        apply(*it->second.second);
      return;
      }
    const auto full = itemDefinitions.fullRestoreEffects.find(functionSymbol->index());
    if(full!=itemDefinitions.fullRestoreEffects.end()) {
      const auto actor = std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
      auto* attribute = findSymbol("C_NPC.ATTRIBUTE");
      vm.set_int(actor,attribute,full->second.attribute,vm.get_int(actor,attribute,full->second.maximum));
      return;
      }
    const auto permanent = itemDefinitions.permanentAttributeEffects.find(functionSymbol->index());
    if(permanent!=itemDefinitions.permanentAttributeEffects.end()) {
      const auto actor = std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
      const auto& effect = permanent->second;
      vm.push_reference(vm.global_self());
      vm.push_int(effect.increase.attribute);
      const auto* dose = std::get_if<uint32_t>(&effect.increase.amount);
      if(dose)
        vm.push_reference(vm.find_symbol_by_index(*dose));
      else
        vm.push_int(std::get<int32_t>(effect.increase.amount));
      vm.unsafe_call_bl(findSymbol("B_RAISEATTRIBUTE"));
      if(effect.refill)
        npc_changeattribute(actor,*effect.refill,amount(effect.increase));
      return;
      }
    if(applyCumulativeFoodEffect(functionSymbol->name()) || applyStockItemUseEffect(functionSymbol->name()) ||
       armorAndSkillEffects() ||
       applyEquipmentStatEffect(*npc,functionSymbol->index()) || applyStatefulEquipmentEffect(*npc,functionSymbol->name()) ||
       invokeStockItemProgram(*functionSymbol))
      return;
    }
  vm.call_function<void>(functionSymbol);
  }

int GameScript::invokeMana(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  if(usesNativeShrinkSpell(npc.activeWeapon()))
    return runShrinkMana(npc,mana,false);
  if(const auto result=runStockSpellMana(npc,mana))
    return *result;
  return vm.call_function<int>(fn,mana);
  }

int GameScript::invokeManaRelease(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana_Release");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  if(usesNativeShrinkSpell(npc.activeWeapon()))
    return runShrinkMana(npc,mana,true);
  if(nativeRoutineProgram && activityEvents && !floorDefer.active() &&
     ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off) {
    const auto actor = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
    findSymbol("SPELL_PROCESSMANA_RELEASE.MANAINVESTED")->set_int(mana);
    const auto spell = npc_getactivespell(actor);
    findSymbol("SPELL_PROCESSMANA_RELEASE.ACTIVESPELL")->set_int(spell);
    for(const auto name : {"SPL_PYROKINESIS", "SPL_CHARGEFIREBALL", "SPL_CHARGEZAP", "SPL_WINDFIST"})
      if(spell==findSymbol(name)->get_int())
        return SPL_SENDCAST;
    return SPL_SENDSTOP;
    }
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
    if(fn->name()=="SPELL_CAST_SHRINK" && usesNativeShrinkSpell(&it)) {
      castShrink(npc,target);
      return;
      }
    if(runStockSpellCast(npc,fn->name(),splLevel))
      return;
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
  int32_t result = 0;
  if(fn->rtype()==daedalus::DaedalusDataType::INT && invokeStockProgram(*fn,result,nullptr,true))
    return result;
  return vm.call_function<int>(fn);
  }

void GameScript::invokePickLock(Npc& npc, int bSuccess, int bBrokenOpen) {
  auto fn   = vm.find_symbol_by_name("G_PickLock");
  if(fn==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && activityEvents && !floorDefer.active() &&
     ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off) {
    runStockPickLock(bSuccess,bBrokenOpen);
    return;
    }
  vm.call_function<void>(fn, bSuccess, bBrokenOpen);
  }

void GameScript::invokeRefreshAtInsert(Npc& npc) {
  LoadProbe::Startup::Detail::Scope detail("refresh_at_insert");
  if(B_RefreshAtInsert==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  if(nativeRoutineProgram && NpcDefinitionRuntime::enabled())
    return refreshNpcAtInsert(npc);
  vm.call_function<void>(B_RefreshAtInsert);
  }

CollideMask GameScript::canNpcCollideWithSpell(Npc& npc, Npc* shooter, int32_t spellId) {
  auto fn   = vm.find_symbol_by_name("C_CanNpcCollideWithSpell");
  if(fn==nullptr)
    return COLL_DOEVERYTHING;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), shooter->handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockSpellCollision(npc,*shooter,spellId);
  return CollideMask(vm.call_function<int>(fn, spellId));
  }

int GameScript::playerHotKeyScreenMap(Npc& pl) {
  auto fn   = vm.find_symbol_by_name("player_hotkey_screen_map");
  if(fn==nullptr) {
    return -1;
    }

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  const int map = nativeRoutineProgram && nativeHumanPerceptionsEnabled() ? stockHeroMap() : vm.call_function<int>(fn);
  if(map>=0)
    pl.useItem(size_t(map));
  return map;
  }

void GameScript::playerHotLamePotion(Npc& pl) {
  auto opt = Settings::getI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_potion");
  if(fn==nullptr)
    return;

  if(Settings::physicsFlags().meleeHealInterrupt && startHotkeyPotion(pl,true))
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockLegacyPotionKey(true);
  vm.call_function<void>(fn);
  }

void GameScript::playerHotLameHeal(Npc& pl) {
  auto opt = Settings::getI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_heal");
  if(fn==nullptr)
    return;

  if(Settings::physicsFlags().meleeHealInterrupt && startHotkeyPotion(pl,false))
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockLegacyPotionKey(false);
  vm.call_function<void>(fn);
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
    if(info->npc==int32_t(npc.instanceSymbol()) && info->important && !info->permanent)
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
  if(nativeRoutineProgram && nativeHumanPerceptionsEnabled())
    return runStockHeroEmptyLoot();
  vm.call_function<void>(id);
  }

void GameScript::useInteractive(const std::shared_ptr<NpcRecord>& hnpc, std::string_view func) {
  auto fn = vm.find_symbol_by_name(func);
  if(fn == nullptr)
    return;

  ScopeVar self(*vm.global_self(),hnpc);
  try {
    if(!invokeNativeMobState(*fn))
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
  onWldInstanceRemoved(&static_cast<const daedalus::IItem&>(itm.handle()));
  }

void GameScript::onWldInstanceRemoved(const daedalus::DaedalusInstance* obj) {
  vm.find_symbol_by_instance(*obj)->set_instance(nullptr);
  }

void GameScript::makeCurrent(Item* w) {
  if(w==nullptr)
    return;
  auto* s = vm.find_symbol_by_index(uint32_t(w->clsId()));
  if(s != nullptr)
    s->set_instance(std::static_pointer_cast<daedalus::IItem>(w->handlePtr()));
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

bool GameScript::hasSymbolName(std::string_view name) {
  return vm.find_symbol_by_name(name)!=nullptr;
  }

uint64_t GameScript::tickCount() const {
  return owner.tickCount();
  }

void GameScript::tick(uint64_t) {
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

std::pair<Npc*,Npc*> GameScript::storyActors() {
  return {findNpc(vm.global_self()),findNpc(vm.global_other())};
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
  auto* handle = nativeRoutineProgram ? storySlot(uint32_t(id)) : vm.find_symbol_by_index(uint32_t(id));
  if(handle==nullptr || !handle->is_instance_of<daedalus::INpc>())
    return nullptr;

  auto hnpc = reinterpret_cast<daedalus::INpc*>(handle->get_instance().get());
  if(hnpc==nullptr) {
    auto obj = world().findNpcByInstance(id);
    handle->set_instance(obj ? std::static_pointer_cast<daedalus::INpc>(obj->handlePtr()) : nullptr);
    hnpc = reinterpret_cast<daedalus::INpc*>(handle->get_instance().get());
    }
  return findNpc(hnpc);
  }

daedalus::IInfo* GameScript::findInfo(size_t id) {
  if(nativeRoutineProgram) {
    const auto entry = storyDialogsById.find(uint32_t(id));
    return entry!=storyDialogsById.end() ? entry->second.second : nullptr;
    }
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
  sym->set_instance(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()));
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
