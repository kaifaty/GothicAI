#include "gamescript.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <limits>

#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/triggers/abstracttrigger.h"
#include "graphics/visualfx.h"
#include "gothic.h"
#include "loadprobe.h"

using namespace Tempest;

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
