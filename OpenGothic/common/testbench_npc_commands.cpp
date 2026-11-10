#include "testbench_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "testbench.h"
#include "npcdayjournal.h"
#include "game/archerai.h"
#include "game/archery.h"
#include "game/gamescript.h"
#include "game/herodeed.h"
#include "game/playercontrol.h"
#include "graphics/mesh/flinch.h"
#include "world/aiqueue.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

bool npcCommandsCommand(const NpcCommandsContext& context,const std::string& line,const std::vector<std::string>& a,
                        World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.npcCommands;
  const auto& c=a[0];
  if(c=="hp") {
    // hp <npc> [full]: hit points of the 3 nearest npcs of that script symbol (sheep are all over the world),
    // and how far (did the arrow hit); full: the nearest gets them all back first
    const std::string name = a.size()>1 ? a[1] : "";
    const size_t      id   = w.script().findSymbolIndex(name);
    std::vector<std::pair<float,Npc*>> list;
    for(uint32_t i=0; i<w.npcCount(); ++i)
      if(Npc* n = w.npcById(i); n!=nullptr && n->instanceSymbol()==id)
        list.emplace_back((n->position()-pl.position()).length(),n);
    std::sort(list.begin(),list.end(),[](auto& x, auto& y) { return x.first<y.first; });
    if(a.size()>2 && a[2]=="full" && !list.empty() && !list[0].second->isDown()) {
      // the nearest gets its hit points back
      Npc& n = *list[0].second;
      n.changeAttribute(ATR_HITPOINTS,n.attribute(ATR_HITPOINTSMAX)-n.attribute(ATR_HITPOINTS),false);
      }
    std::string out = "hp " + name + ":";
    for(size_t i=0; i<list.size() && i<3; ++i)
      out += " " + std::to_string(list[i].second->attribute(ATR_HITPOINTS)) + (list[i].second->isDead() ? " dead" : "") +
             " at " + std::to_string(int(list[i].first/100.f)) + " m;";
    say(out);
    }
  else if(c=="give" || c=="equip") {
    // give <item> [count]: into the hero's inventory; equip <item>: give one and put it on (a bow, a rune)
    const std::string name = a.size()>1 ? a[1] : "";
    const size_t      id   = w.script().findSymbolIndex(name);
    if(id==size_t(-1)) {
      say(c + ": no item " + name);
      return true;
      }
    pl.addItem(id,size_t(c=="give" ? num(a,2,1) : 1));
    if(c=="equip")
      pl.useItem(id,Item::NSLOT,true); // whatever the hero's strength
    healingObserve(context.healing,w,pl);
    say(c + " " + name);
    }
  else if(c=="disarm") {
    // disarm: deterministic bench setup, with no weapon-key toggle or holstering animation.
    ctrl.clearInput();
    context.holds.clear();
    context.taps.clear();
    pl.endManualAim(false);
    pl.closeWeapon(true);
    pl.clearState(true);
    pl.clearAiQueue();
    pl.clearSpeed();
    pl.stopAnim("");
    say("disarm ws=" + std::to_string(int(pl.weaponState())));
    }
  else if(c=="draw") {
    // draw bow: draw the equipped bow/crossbow; calling it again never puts the weapon away.
    if(a.size()!=2 || a[1]!="bow") {
      say("draw: expected draw bow");
      return true;
      }
    const bool ok = pl.currentRangedWeapon()!=nullptr && pl.drawWeaponBow();
    say(std::string("draw bow ") + (ok ? "ok" : "failed") + " ws=" + std::to_string(int(pl.weaponState())));
    }
  else if(c=="put" || c=="put-at") {
    // put <npc|nearest> <dist> <angle> [dy]: the npc stands there from the hero (angle from the hero's
    // facing, 0 in front), on the ground, frozen, facing the hero
    // put-at <npc|player> <x> <y> <z>: the same ground placement at explicit world coordinates.
    Npc* n = c=="put-at" && a.size()>1 && a[1]=="player" ? &pl : findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || (n==&pl && c!="put-at")) {
      say("put: no npc " + line);
      return true;
      }
    const float d   = num(a,2,500);
    const float an  = (pl.rotation()+num(a,3,0))*float(M_PI)/180.f;
    Vec3        pos = pl.position() + Vec3(std::cos(an),0,std::sin(an))*d;
    if(c=="put-at") {
      if(a.size()!=5) {arenaAbort("put-at needs npc x y z");return true;}
      pos=Vec3(num(a,2,0),num(a,3,0),num(a,4,0));
      n->clearSpeed();
      }
    // the ground there may be far above or below (a slope): the first one from above, by short rays
    // (a long one misses the landscape)
    DynamicWorld::RayLandResult r;
    for(float y=pos.y+(c=="put-at" ? 0.f : num(a,4,0))+1500; y>pos.y-1500 && !r.hasCol; y-=400)
      r = w.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
    if(c=="put-at" && !r.hasCol) {arenaAbort("put-at found no ground");return true;}
    if(r.hasCol)
      pos.y = r.v.y;
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    if(n!=&pl) n->setExternalControl(true);
    n->setPosition(pos);
    const Vec3 dir = pl.position()-pos;
    if(n!=&pl) n->setDirection(Vec3(dir.x,0,dir.z));
    n->updateTransform();
    char buf[200];
    std::snprintf(buf,sizeof(buf),"put %s at %.0f %.0f %.0f (%.0f above the hero%s)",symName(w,n).c_str(),pos.x,pos.y,pos.z,
                  pos.y-pl.position().y,r.hasCol ? "" : ", no ground found");
    say(buf);
    }
  else if(c=="pin") {
    // pin <npc|nearest> <dist> <angle> <height> | pin off: the npc hangs there from the hero (angle from
    // the hero's facing, height above the hero's feet), frozen, every frame: a target above or below
    if(a.size()>1 && a[1]=="off") {
      context.pinned = nullptr;
      say("pin off");
      return true;
      }
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("pin: no npc " + line);
      return true;
      }
    n->setInteraction(nullptr,true);
    n->clearAiQueue();
    n->clearState(false);
    n->setExternalControl(true);
    const float an = (pl.rotation()+num(a,3,0))*float(M_PI)/180.f;
    context.pinned   = n;
    context.pinAt    = pl.position() + Vec3(std::cos(an)*num(a,2,800),num(a,4,0),std::sin(an)*num(a,2,800));
    char buf[200];
    std::snprintf(buf,sizeof(buf),"pin %s at %.0f %.0f %.0f",symName(w,n).c_str(),context.pinAt.x,context.pinAt.y,context.pinAt.z);
    say(buf);
    }
  else if(c=="npcgo") {
    // npcgo <npc|nearest> <waypoint> [walk|run]: the npc goes there (AI path), like a script sends it
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    auto wp = a.size()>2 ? w.findPoint(a[2]) : nullptr;
    if(n==nullptr || n==&pl || wp==nullptr) {
      say("npcgo: no npc or waypoint " + line);
      return true;
      }
    n->setWalkMode(a.size()>3 && a[3]=="walk" ? WalkBit::WM_Walk : WalkBit::WM_Run);
    n->aiPush(AiQueue::aiGoToPoint(*wp));
    say("npcgo " + symName(w,n) + " " + a[2]);
    }
  else if(c=="stats") {
    // stats [npc <npc>] [str <n>] [dex <n>] [1h <n>] [2h <n>] [bow <n>] [cbow <n>] [sneak <n>] [hp <n>] [hpmax <n>]
    // [mana <n>] [circle <n>] [talent auto|0|1|2]:
    // the hero's (an npc's) strength, dexterity, hit chances (the skill of the manual aim), hit points, mana (and its
    // maximum), the magic circle;
    // talent: archery animations, plus melee weapons explicitly named in this command; auto follows hit chances
    Npc*   np = &pl;
    size_t i0 = 1;
    if(a.size()>1 && a[1]=="npc") {
      np = a.size()>2 ? findNpc(w,pl,a[2]) : nullptr;
      if(np==nullptr) {
        say("stats: no npc " + line);
        return true;
        }
      i0 = 3;
      }
    Npc&   n    = *np;
    auto&  h    = n.handle();
    bool   more = &n!=&pl;
    bool oneHand = false, twoHand = false;
    for(size_t i=i0; i+1<a.size(); i+=2) {
      oneHand |= a[i]=="1h";
      twoHand |= a[i]=="2h";
      }
    for(size_t i=i0; i+1<a.size(); i+=2) {
      if(a[i]=="talent") {
        auto level = [&](int32_t chance) {
          if(a[i+1]=="auto")
            return chance>=60 ? 2 : (chance>=30 ? 1 : 0);
          return int(num(a,i+1,0));
          };
        if(oneHand)
          n.setTalentSkill(TALENT_1H,level(h.hitchance[TALENT_1H]));
        if(twoHand)
          n.setTalentSkill(TALENT_2H,level(h.hitchance[TALENT_2H]));
        n.setTalentSkill(TALENT_BOW,     level(h.hitchance[TALENT_BOW]));
        n.setTalentSkill(TALENT_CROSSBOW,level(h.hitchance[TALENT_CROSSBOW]));
        more = true;
        continue;
        }
      const int v = int(num(a,i+1,0));
      if(a[i]=="str")
        h.attribute[ATR_STRENGTH] = v;
      else if(a[i]=="dex")
        h.attribute[ATR_DEXTERITY] = v;
      else if(a[i]=="1h")
        h.hitchance[TALENT_1H] = v;
      else if(a[i]=="2h")
        h.hitchance[TALENT_2H] = v;
      else if(a[i]=="bow")
        h.hitchance[TALENT_BOW] = v;
      else if(a[i]=="cbow")
        h.hitchance[TALENT_CROSSBOW] = v;
      else if(a[i]=="sneak")
        n.setTalentSkill(TALENT_SNEAK,v);
      else if(a[i]=="hp")
        h.attribute[ATR_HITPOINTS] = v;
      else if(a[i]=="hpmax")
        h.attribute[ATR_HITPOINTSMAX] = v;
      else if(a[i]=="mana")
        h.attribute[ATR_MANA] = h.attribute[ATR_MANAMAX] = v;
      else if(a[i]=="circle")
        n.setTalentSkill(TALENT_MAGE,v);
      else
        say("stats: unknown " + a[i]);
      }
    const auto  wp = n.currentRangedWeapon();
    const auto  st = Archery::stats(n,wp);
    char buf[520];
    if(more) {
      // whose, the reload as long as the animation is (the trained ones' is shorter), the hit points
      const bool cbow = wp!=nullptr && wp->isCrossbow();
      std::snprintf(buf,sizeof(buf),"stats of %s: 1h %d (talent %d), 2h %d (talent %d), talent bow %d cbow %d, reload %.0f ms, hp %d of %d, point protection %d",
                    symName(w,&n).c_str(),n.hitChance(TALENT_1H),n.talentSkill(TALENT_1H),
                    n.hitChance(TALENT_2H),n.talentSkill(TALENT_2H),n.talentSkill(TALENT_BOW),n.talentSkill(TALENT_CROSSBOW),
                    double(n.animTime(Archery::reloadAnim(cbow))),n.attribute(ATR_HITPOINTS),n.attribute(ATR_HITPOINTSMAX),
                    n.protection(PROT_POINT));
      say(buf);
      }
    // an npc's: by name (the report takes `stats str` for the hero's)
    const std::string who = &n!=&pl ? " npc " + symName(w,&n) : "";
    std::snprintf(buf,sizeof(buf),"stats%s str %d dex %d bow %d cbow %d, weapon damage %d: skill %.2f strength %.2f sway %.2f deg settle %.2f s breath %.1f s draw %.2f s hold %.1f s speed %.1f m/s cone %.2f deg reload x%.2f need %.2f r %.3f f_s %.3f f_b %.3f",
                  who.c_str(),n.attribute(ATR_STRENGTH),n.attribute(ATR_DEXTERITY),n.hitChance(TALENT_BOW),n.hitChance(TALENT_CROSSBOW),
                  wp!=nullptr ? int(wp->handle().damage_total) : -1,
                  double(st.skill),double(st.strength),double(st.sway*180.f/float(M_PI)),double(st.settle),double(st.breath),
                  double(st.drawTime),double(std::min(st.holdLimit,999.f)),double(st.speed*10.f),double(st.cone*180.f/float(M_PI)),
                  double(st.reload),double(st.need),double(st.strength),double(st.steadyDraw),double(st.burstDraw));
    say(buf);
    }
  else if(c=="npcday") {
    // npcday <game minutes> [period minutes 5] [shared]: journal every NPC over continuous game time
    // (common/npcdayjournal.h) into npc-day*.csv; the scenario waits until the time has passed. Each NPC gets
    // a private script random stream from the last `seed`, unless `shared`.
    NpcDayJournal::Options opt;
    opt.directory = outDir;
    opt.seed      = context.scenarioSeed;
    size_t positional = 0;
    bool   ok = a.size()>1;
    for(size_t i=1; i<a.size(); ++i) {
      if(a[i]=="shared")
        opt.isolate = false;
      else if(isNumber(a[i]) && positional==0 && num(a,i,0)>0)
        opt.minutes = uint64_t(num(a,i,0)), ++positional;
      else if(isNumber(a[i]) && positional==1 && num(a,i,0)>0)
        opt.period = uint32_t(num(a,i,0)), ++positional;
      else
        ok = false;
      }
    std::string error;
    if(!ok || !NpcDayJournal::start(w,opt,error)) {
      say("npcday failed: "+(ok ? error : "usage: npcday <game minutes> [period minutes] [shared]"));
      event("npcday_error","\"reason\":"+jsonStr(ok ? error : "usage"));
      return true;
      }
    state.npcDayStart = w.time().toInt();
    state.npcDayMinutes = opt.minutes;
    say("npcday "+std::to_string(opt.minutes)+" min every "+std::to_string(opt.period)+(opt.isolate ? "" : " shared"));
    event("npcday_start","\"minutes\":"+std::to_string(opt.minutes)+",\"period\":"+std::to_string(opt.period)+
          ",\"isolate\":"+(opt.isolate ? "true" : "false")+",\"seed\":"+std::to_string(opt.seed));
    }
  else if(c=="seed") {
    // seed <n>: the random numbers start over: the scripts' (the dice, the release), the bench's, the
    // hero's sway
    const auto* live=std::getenv("OPENGOTHIC_NAV_LIVE");
    // The live suite's adjacent seeds exceed the exact integer range of float.
    const uint32_t s = live!=nullptr && std::string_view(live)=="1" ?
                       uint32_t(std::stoul(a.at(1))) : uint32_t(num(a,1,1));
    w.script().setSeed(s);
    context.scenarioSeed = s;
    context.lcg = s*2654435761u + 1u;
    context.monsterCharacterSeed=s;
    pl.archer().reseed(s);
    say("seed " + std::to_string(s));
    }
  else if(c=="npcequip") {
    // npcequip <npc> <item> [count]: into its inventory, the first one put on (a bow and its arrows)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    const size_t id = w.script().findSymbolIndex(a.size()>2 ? a[2] : "");
    if(n==nullptr || id==size_t(-1)) {
      say("npcequip: no npc or item " + line);
      return true;
      }
    n->addItem(id,size_t(num(a,3,1)));
    n->useItem(id,Item::NSLOT,true);
    say("npcequip " + symName(w,n) + " " + a[2]);
    }
  else if(c=="wander" && a.size()>1 && a[1]=="stats") {
    size_t walking = 0;
    for(auto* n:state.wanderers)
      if(!n->isDown() && n->isAiBusy())
        ++walking;
    say("wander stats: " + std::to_string(state.wanderers.size()) + " npcs, " + std::to_string(walking) +
        " walking, " + std::to_string(state.wanderLegs) + " legs");
    }
  else if(c=="stuck") {
    // stuck begin <label> [radius cm 3000] | stuck end: observe native routine movement without
    // taking control of any NPC. The logger is absent unless OPENGOTHIC_STUCK_LOG is set.
    if(a.size()>=3 && a.size()<=4 && a[1]=="begin") {
      const float radius = num(a,3,3000);
      if(!std::isfinite(radius) || radius<=0 || (a.size()==4 && !isNumber(a[3]))) {
        say("stuck begin failed: radius must be positive and finite");
        return true;
        }
      const bool ok = TestBench::stuckBegin && TestBench::stuckBegin(w,a[2],radius);
      say(std::string(ok ? "stuck begin " : "stuck begin failed: ") + a[2] +
          " radius_cm=" + std::to_string(radius));
      }
    else if(a.size()==2 && a[1]=="end") {
      const bool ok = TestBench::stuckEnd && TestBench::stuckEnd(w);
      say(ok ? "stuck end" : "stuck end failed: logger unavailable or no observation");
      }
    else {
      say("stuck: expected begin <label> [radius cm 3000] or end");
      }
    }
  else if(c=="wander") {
    // wander [radius cm 3000] [walk|run]: each npc set out by `arc` is held as the AI server holds a body and
    // walks by the waynet to a waypoint within the radius of the hero, then to another: a crowd for the speed
    // probe (docs/navigation.md, stage 0)
    const float r = num(a,1,3000);
    state.wanderPoints.clear();
    w.findWayPoint(pl.position(),[&](const WayPoint& p) {
      if((p.pos-pl.position()).quadLength()<r*r)
        state.wanderPoints.push_back(&p);
      return false;
      });
    const bool run = a.size()>2 && a[2]=="run";
    state.wanderers.clear();
    state.wanderLegs = 0;
    for(auto* n:context.crew) {
      n->clearAiQueue();
      n->clearState(false);
      n->setExternalControl(true);
      n->setWalkMode(run ? WalkBit::WM_Run : WalkBit::WM_Walk);
      state.wanderers.push_back(n);
      }
    say("wander " + std::to_string(state.wanderers.size()) + " npcs, " + std::to_string(state.wanderPoints.size()) +
        " waypoints within " + std::to_string(int(r)) + " cm");
    }
  else if(c=="fight") {
    // fight <npc|all> <target|player> [deed]: deed uses npc_act's nonlethal native start.
    std::vector<Npc*> who;
    if(a.size()>1 && a[1]=="all")
      who = context.crew; else
    if(Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest"))
      who.push_back(n);
    Npc*         tg     = findNpc(w,pl,a.size()>2 ? a[2] : "player");
    const size_t state  = w.script().findSymbolIndex("ZS_ATTACK");
    const bool   deed   = a.size()==4 && a[3]=="deed";
    auto         reason = w.script().findSymbol("AIV_ATTACKREASON");
    auto         kill   = w.script().findSymbol("AR_KILL");
    if(tg==nullptr || state==size_t(-1)) {
      say("fight: no target or no ZS_ATTACK " + line);
      return true;
      }
    if(a.size()>3 && !deed)
      throw std::runtime_error("fight expects actor target [deed]");
    if(deed && (tg->isPlayer() || tg->isMonster() || tg->isDown()))
      throw std::runtime_error("fight deed needs a standing human target");
    size_t sent = 0;
    for(auto n:who) {
      if(n==tg || n->isDown())
        continue;
      if(deed) {
        auto* none = w.script().findSymbol("AR_NONE");
        const auto attack = HeroDeed::constant(*n,"AIV_ATTACKREASON");
        const auto fight = HeroDeed::constant(*n,"AIV_LASTFIGHTAGAINSTPLAYER");
        const auto comment = HeroDeed::constant(*n,"AIV_LASTFIGHTCOMMENT");
        const auto valid = [](int32_t i) { return i>=0 && uint32_t(i)<daedalus::INpc::aivar_count; };
        if(none==nullptr || !none->is_const() || none->type()!=daedalus::DaedalusDataType::INT ||
           !valid(attack) || !valid(fight) || !valid(comment))
          throw std::runtime_error("fight deed needs native attack variables and AR_NONE");
        auto& aiv = n->handle().aivar;
        aiv[size_t(attack)] = none->get_int(0);
        aiv[size_t(fight)] = HeroDeed::constant(*n,"FIGHT_CANCEL",0);
        aiv[size_t(comment)] = 0;
        n->stopListening();
        n->thinkOfAnswer(0);
        n->clearAiQueue();
        n->setExternalControl(false);
        if(!n->startState(ScriptFn(state),"",gtime::endOfTime(),true))
          throw std::runtime_error("fight deed could not start ZS_ATTACK");
        n->setOther(tg);
        n->setVictim(nullptr);
        n->setTarget(tg);
        n->setNonlethalFight(*tg);
        n->setServerDeed("attack",tg);
        if(n->weaponState()==WeaponState::NoWeapon)
          n->aiPush(AiQueue::aiDrawWeapon());
        }
      else {
        if(reason!=nullptr && kill!=nullptr && size_t(reason->get_int())<std::size(n->handle().aivar))
          n->handle().aivar[size_t(reason->get_int())] = kill->get_int();
        n->setExternalControl(false);
        n->setTarget(tg);
        n->aiPush(AiQueue::aiStartState(ScriptFn(state),0,tg,nullptr,""));
        }
      ++sent;
      }
    say("fight " + std::to_string(sent) + " at " + symName(w,tg) + (deed ? " deed" : ""));
    event("fight_start","\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"seed\":"+std::to_string(context.scenarioSeed)+",\"target\":"+jsonStr(symName(w,tg))+
          ",\"deed\":"+(deed ? "true" : "false"));
    }
  else if(c=="waitdown") {
    // waitdown <npc|player> [ms] [other npc]: wait till either participant is down.
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("waitdown: no npc " + line);
      return true;
      }
    state.downOther = a.size()>3 ? findNpc(w,pl,a[3]) : nullptr;
    if(a.size()>3 && state.downOther==nullptr)
      throw std::runtime_error("waitdown has no second participant");
    state.downNpc  = n;
    state.downFrom = context.time;
    state.downEnd  = context.time + uint64_t(num(a,2,120000));
    }
  else if(c=="npcaim") {
    // npcaim on [second|arrow] | off | sigma <deg> | sigma off | lead on|off: npc archers aim by hand
    // (game/archerai): as many hits a second as G2's dice (second) or as sure an arrow (arrow); an error
    // forced (degrees per axis); the lead on a runner
    const std::string m = a.size()>1 ? a[1] : "";
    ++ArcherAi::epoch;
    if(m=="on") {
      ArcherAi::setEnabled(true);
      if(a.size()>2)
        ArcherAi::mode = a[2]=="arrow" ? ArcherAi::PerArrow : ArcherAi::PerSecond;
      }
    else if(m=="off")
      ArcherAi::setEnabled(false);
    else if(m=="sigma")
      ArcherAi::forcedSigma = a.size()>2 && a[2]!="off" ? num(a,2,0) : -1.f;
    else if(m=="lead")
      ArcherAi::leadOn = a.size()<3 || a[2]!="off";
    say("npcaim " + std::string(ArcherAi::isEnabled() ? "on" : "off") + (ArcherAi::mode==ArcherAi::PerArrow ? " arrow" : " second") +
        " sigma " + std::to_string(ArcherAi::forcedSigma) + " lead " + (ArcherAi::leadOn ? "on" : "off"));
    }
  else if(c=="still") {
    // still <npc|player> on|off: hit, it neither stumbles nor flinches (a target that keeps its pose, as
    // its zones were dumped)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("still: no npc " + line);
      return true;
      }
    n->setBenchStill(a.size()<3 || a[2]!="off");
    say("still " + symName(w,n) + (n->isBenchStill() ? " on" : " off"));
    }
  else if(c=="zones" && a.size()>2 && a[1]=="fit") {
    // zones fit on|off: the hit zones as thick as the body's mesh, or by a guess from the bone names (the
    // hero refitted now, the others when they change their body)
    HitZones::setFitEnabled(a[2]=="on");
    pl.fitHitZones();
    say("zones fit " + a[2]);
    }
  else if(c=="zones") {
    // zones <npc|nearest|player>: where its hit zones are now (the capsules on its bones), how well they
    // cover its body
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr) {
      say("zones: no npc " + line);
      return true;
      }
    Vec3 h, b;
    n->hitZoneCenter(HitZones::Head,h);
    n->hitZoneCenter(HitZones::Body,b);
    char buf[200];
    std::snprintf(buf,sizeof(buf),"zones %s at %.0f %.0f %.0f: head %.0f %.0f %.0f body %.0f %.0f %.0f",symName(w,n).c_str(),
                  n->position().x,n->position().y,n->position().z,h.x,h.y,h.z,b.x,b.y,b.z);
    say(buf + std::string("\n") + n->hitZonesDump());
    }
  else if(c=="open") {
    // open [dist cm]: how far it is open around the hero at head height and at the knees, every 15
    // degrees from the facing (a target for the bow needs open ground)
    const float dist = num(a,1,4000);
    const Vec3  p0   = pl.position();
    std::string out  = "open (yaw from the facing: head/knees m):";
    for(int i=0; i<24; ++i) {
      const float an = (pl.rotation()+float(i*15))*float(M_PI)/180.f;
      const Vec3  d  = Vec3(std::cos(an),0,std::sin(an));
      float       m[2];
      for(int k=0; k<2; ++k) {
        const Vec3 from = p0 + Vec3(0,k==0 ? 160.f : 40.f,0);
        auto       r    = w.physic()->ray(from,from+d*dist);
        m[k] = r.hasCol ? (r.v-from).length()/100.f : dist/100.f;
        }
      char buf[48];
      std::snprintf(buf,sizeof(buf)," %d:%.0f/%.0f",i*15,double(m[0]),double(m[1]));
      out += buf;
      }
    say(out);
    }
  else if(c=="volley") {
    // volley <shots> [hold ms] [draw 0..1] [jitter ms]: shots by hand in a row: the string drawn
    // that far (1: full), held (up to jitter longer, at random), let go; the scenario goes on after the last
    // one is reloaded. Held only at the archer's own limit: short of it the string goes as it is reached
    state.volleyLeft  = int(num(a,1,5));
    state.volleyHold  = uint64_t(num(a,2,300));
    state.volleyDraw  = num(a,3,1);
    state.volleyJitter = uint64_t(num(a,4,0));
    state.volleyPhase = 0;
    state.volleyT     = context.time;
    say("volley " + std::to_string(state.volleyLeft));
    }
  else if(c=="immortal") {
    // immortal <npc|nearest> on|off: arrows hit it, it does not die (counting hits)
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr) {
      say("immortal: no npc " + line);
      return true;
      }
    auto& h = n->handle();
    if(a.size()>2 && a[2]=="off")
      h.flags = daedalus::NpcFlag(uint32_t(h.flags) & ~uint32_t(daedalus::NpcFlag::IMMORTAL)); else
      h.flags = daedalus::NpcFlag(uint32_t(h.flags) |  uint32_t(daedalus::NpcFlag::IMMORTAL));
    say("immortal " + symName(w,n) + " " + (n->isImmortal() ? "on" : "off"));
    }
  else if(c=="stun-drift") {
    auto* target=findNpc(w,pl,a.size()>1 ? a[1] : "");
    auto* attacker=a.size()>2 && a[2]=="player" ? &pl : findNpc(w,pl,a.size()>2 ? a[2] : "");
    if(target==nullptr || attacker==nullptr || target==attacker) {arenaAbort("stun-drift needs a target and attacker");return true;}
    auto axis=attacker->position()-target->position();axis.y=0;
    if(axis.length()<1.f) {arenaAbort("stun-drift needs distinct horizontal roots");return true;}
    state.stunDrift={target,attacker,target->position(),Vec3::normalize(axis),w.tickCount(),1};
    context.stunDriftSample(w,0,0);
    }
  else if(c=="stun") {
    // stun <npc|nearest|player> [parried|guard_break|posture]: melee stun, by the hero
    // (or, for the hero, by the nearest npc): the look without timing a real parade
    Npc* n = a.size()>1 && a[1]=="player" ? &pl : findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    Npc* by = n==&pl ? findNpc(w,pl,"nearest") : &pl;
    if(n==nullptr || by==nullptr) {
      say("stun: no npc " + line);
      return true;
      }
    n->startMeleeStun(*by,a.size()>2 ? a[2] : "parried");
    say("stun " + symName(w,n) + (n->isMeleeStunned() ? " on" : " refused"));
    }
  else if(c=="dodge") {
    // dodge [<npc>] back|left|right|forward|none [before <attacker> <ms>]: the melee dodge (melee_dodge) that way
    // relative to its facing, now, or as the attacker's next blow comes within <ms> of its contact (8 s at
    // most). Its line in the log: the way, the armour, the move and animation or fallback, cost, window. The
    // hero's own key is `tap dodge` (the way from the movement keys held then)
    size_t i = 1;
    Npc*   n = &pl;
    MeleeDodge::Direction dir = MeleeDodge::Direction::Back;
    if(a.size()>i && !dodgeDirection(a[i],dir)) {
      n = findNpc(w,pl,a[i]);
      ++i;
      }
    if(n==nullptr || a.size()<=i || !dodgeDirection(a[i],dir)) {
      say("dodge: no npc or way " + line);
      return true;
      }
    ++i;
    if(a.size()>i && a[i]=="before") {
      Npc* by = a.size()>i+1 ? findNpc(w,pl,a[i+1]) : nullptr;
      if(by==nullptr || by==n) {
        say("dodge: no attacker " + line);
        return true;
        }
      context.melee.dodgeArm = Melee::DodgeArm{n,by,dir,uint64_t(std::max(0.f,num(a,i+2,120))),w.tickCount()+8000};
      say("dodge " + symName(w,n) + " " + std::string(MeleeDodge::name(dir)) + " armed: " +
          std::to_string(context.melee.dodgeArm->leadMs) + " ms before the blow of " + symName(w,by));
      return true;
      }
    const bool ok = n->meleeDodge(dir);
    say("dodge " + symName(w,n) + " " + std::string(MeleeDodge::name(dir)) + (ok ? ": started" : ": not started"));
    }
  else if(c=="clear") {
    // clear [radius]: the npcs around the hero leave the world (rats, wolves that would join in), not the
    // watched one
    const float r = num(a,1,2500);
    std::vector<Npc*> gone;
    for(uint32_t i=0; i<w.npcCount(); ++i)
      if(Npc* n = w.npcById(i); n!=nullptr && n!=&pl && n!=context.subject && (n->position()-pl.position()).length()<r)
        gone.push_back(n);
    std::string out = "clear:";
    for(auto n:gone) {
      out += " " + symName(w,n);
      context.crew.erase(std::remove(context.crew.begin(),context.crew.end(),n),context.crew.end());
      context.archery.archers.erase(std::remove_if(context.archery.archers.begin(),context.archery.archers.end(),[n](auto& x){ return x.first==n; }),context.archery.archers.end());
      if(context.archery.archerAt==n) {
        // their target gone: archer off
        for(auto& [x,at]:context.archery.archers)
          if(x!=nullptr) {
            x->archerAi().cancel(*x);
            x->setExternalControl(false);
            }
        context.archery.archers.clear();
        context.archery.archerAt = nullptr;
        }
      if(context.pinned==n)
        context.pinned = nullptr;
      if(state.downNpc==n)
        state.downNpc = nullptr;
      if(state.downOther==n)
        state.downOther = nullptr;
      if(context.melee.dodgeArm && (context.melee.dodgeArm->npc==n || context.melee.dodgeArm->attacker==n))
        context.melee.dodgeArm.reset();
      if(context.melee.meleeAiResponse && (context.melee.meleeAiResponse->npc==n || context.melee.meleeAiResponse->attacker==n))
        context.melee.meleeAiResponse.reset();
      w.removeNpc(*n);
      }
    say(out);
    }
  else if(c=="flinch") {
    // flinch on|off: the upper body gives way to a blow (graphics/mesh/flinch)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Flinch::setEnabled(m=="on"); else
      say("unknown flinch command: " + line);
    }
  else if(c=="hit") {
    // hit <npc|player> <angle> [strength] [height] [twist]: a blow from that side of it (angle from its
    // facing, 0 in front, as place), only the flinch: no damage, no stumble
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    if(n==nullptr) {
      say("hit: no npc " + line);
      return true;
      }
    const float an = (n->rotation()+num(a,2,0))*float(M_PI)/180.f;
    Flinch::Hit h;
    h.dir      = Vec3(-std::cos(an),0,-std::sin(an));
    h.strength = num(a,3,1);
    h.height   = num(a,4,0.6f);
    h.twist    = num(a,5,0);
    n->flinch(h);
    char buf[160];
    std::snprintf(buf,sizeof(buf),"hit %s from %.0f strength %.2f height %.2f twist %.2f",symName(w,n).c_str(),
                  double(num(a,2,0)),double(h.strength),double(h.height),double(h.twist));
    say(buf);
    }
  else if(c=="strike") {
    // strike <victim|player> [attacker|player]: the attacker's blow lands as the game deals it (melee: the
    // stumble, the damage, the flinch); the victim gets its hit points back below a third
    Npc* n  = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    Npc* by = findNpc(w,pl,a.size()>2 ? a[2] : "player");
    if(n==nullptr || by==nullptr || n==by) {
      say("strike: no npc " + line);
      return true;
      }
    const int32_t hp0 = n->attribute(ATR_HITPOINTS);
    n->takeDamage(*by,nullptr);
    const int32_t hp1 = n->attribute(ATR_HITPOINTS);
    say("strike " + symName(w,n) + " by " + symName(w,by) + ": hp " + std::to_string(hp0) + " -> " + std::to_string(hp1) +
        " anim " + n->animNames());
    if(!n->isDown() && hp1*3<n->attribute(ATR_HITPOINTSMAX))
      n->changeAttribute(ATR_HITPOINTS,n->attribute(ATR_HITPOINTSMAX)-hp1,false);
    }
  else return false;
  return true;
  }

}
