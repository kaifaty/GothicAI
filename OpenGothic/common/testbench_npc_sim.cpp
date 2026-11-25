#include "testbench_internal.h"

#include "game/herodeed.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "gothic.h"
#include "settings.h"
#include "world/benchhooks.h"
#include "world/aiqueue.h"
#include "world/npcsimtier.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void NpcDuel::hit(Npc& victim, Npc& by, int32_t loss, std::string_view) {
  if(!active || loss<=0) return;
  for(size_t i=0;i<2;++i)
    if(actors[i]==&by && actors[1-i]==&victim) {
      ++hits[i]; damage[i]+=loss;
      if(loser<0 && victim.isDown())loser=int(1-i);
      }
  }

namespace {

// Raw script arrays match story_eval; weapon arrays are retained separately, without combining them.
template<class T>
std::string npcDuelArray(const T& values) {
  std::string out="[";
  for(const auto value:values) { if(out.size()>1)out+=','; out+=std::to_string(value); }
  return out+"]";
  }

}

std::string npcDuelSnapshot(World& world, Npc& npc) {
  const auto& h=npc.handle();
  auto* weapon=npc.currentMeleeWeapon();
  const auto weaponName=weapon ? world.script().storyItemName(uint32_t(weapon->clsId())) : std::string_view{};
  std::string skills="[", values="[";
  for(int i=0;i<TALENT_MAX_G2;++i) {
    if(i>0) {skills+=',';values+=',';}
    skills+=std::to_string(npc.talentSkill(Talent(i)));
    values+=std::to_string(npc.talentValue(Talent(i)));
    }
  return "\"instance\":"+jsonStr(symName(world,&npc))+",\"level\":"+std::to_string(h.level)+
    ",\"hp_before\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
    ",\"hp_max\":"+std::to_string(npc.attribute(ATR_HITPOINTSMAX))+
    ",\"str\":"+std::to_string(npc.attribute(ATR_STRENGTH))+",\"dex\":"+std::to_string(npc.attribute(ATR_DEXTERITY))+
    ",\"hitchance\":{\"fist\":"+std::to_string(npc.hitChance(TALENT_UNKNOWN))+
    ",\"1h\":"+std::to_string(npc.hitChance(TALENT_1H))+",\"2h\":"+std::to_string(npc.hitChance(TALENT_2H))+"}"+
    ",\"talent_skills\":"+skills+"],\"talent_values\":"+values+"]"+
    ",\"protection\":"+npcDuelArray(h.protection)+",\"damage\":"+npcDuelArray(h.damage)+
    ",\"damagetype\":"+std::to_string(h.damage_type)+",\"weapon\":"+jsonStr(weaponName)+
    ",\"two_handed\":"+(weapon && weapon->is2H() ? "true" : "false")+
    ",\"weapon_damage\":"+(weapon ? npcDuelArray(weapon->handle().damage) : "[0,0,0,0,0,0,0,0]")+
    ",\"weapon_damage_total\":"+std::to_string(weapon ? weapon->handle().damage_total : 0)+
    ",\"weapon_damagetype\":"+std::to_string(weapon ? weapon->handle().damage_type : 0)+
    ",\"immortal_before\":"+(npc.isImmortal() ? "true" : "false")+
    ",\"guild\":"+std::to_string(h.guild)+",\"fight_tactic\":"+std::to_string(h.fight_tactic);
  }

void NpcFightTrace::hit(Npc& victim, Npc& by, int32_t loss, std::string_view kind) {
  const auto attacker = by.creationSeq(), target = victim.creationSeq();
  const bool selectedAttacker = std::find(actors.begin(),actors.end(),attacker)!=actors.end();
  const bool selectedVictim = std::find(actors.begin(),actors.end(),target)!=actors.end();
  if(!selectedAttacker && !selectedVictim)
    return;
  const auto hp = victim.attribute(ATR_HITPOINTS);
  event("npc_fight_record_hit","\"label\":"+jsonStr(label)+",\"t_game_ms\":"+std::to_string(victim.world().tickCount())+
        ",\"attacker\":"+std::to_string(attacker)+",\"victim\":"+std::to_string(target)+",\"damage_kind\":"+jsonStr(kind)+
        ",\"external\":"+(selectedAttacker && selectedVictim ? "false" : "true")+
        ",\"hp_before\":"+std::to_string(int64_t(hp)+loss)+",\"hp_after\":"+std::to_string(hp)+
        ",\"hp_loss\":"+std::to_string(loss));
  }

void NpcFightTrace::stop() {
  Gothic::inst().onCombatHit.ubind(this,&NpcFightTrace::hit);
  actors.clear();
  }

void npcFightTraceCommand(NpcFightTrace& trace,const std::vector<std::string>& args,const std::vector<Npc*>& crew,
                          World& world,Npc& player,uint64_t seed) {
  const auto now = world.tickCount();
  if(args.size()==2 && args[1]=="end") {
    if(trace.actors.empty())
      throw std::runtime_error("npcfight-record end needs an active trace");
    if(now<trace.from)
      throw std::runtime_error("npcfight-record cannot span a rewind of the game clock");
    std::string rows = "[";
    for(const auto id:trace.actors) {
      if(rows.size()>1)
        rows += ',';
      rows += "{\"id\":"+std::to_string(id);
      Npc* npc = nullptr;
      for(uint32_t i=0; i<world.npcCount(); ++i)
        if(auto* found=world.npcById(i); found!=nullptr && found->creationSeq()==id) {
          npc = found;
          break;
          }
      if(npc==nullptr) {
        rows += ",\"present\":false}";
        continue;
        }
      rows += ",\"present\":true,\"hp_after\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+
              ",\"down\":"+(npc->isDown() ? "true" : "false")+
              ",\"dead\":"+(npc->isDead() ? "true" : "false")+
              ",\"position_cm\":"+ragdollVector(npc->position())+"}";
      }
    event("npc_fight_record_end","\"label\":"+jsonStr(trace.label)+",\"t_game_ms\":"+std::to_string(now)+
          ",\"duration_ms\":"+std::to_string(now-trace.from)+",\"participants\":"+rows+"]");
    trace.stop();
    return;
    }
  if(args.size()<4 || args[1]!="begin" || !trace.actors.empty())
    throw std::runtime_error("npcfight-record expects begin label actor... or end; close any active trace first");

  std::vector<uint32_t> actors;
  std::string rows = "[";
  const auto add = [&](Npc* npc) {
    if(npc==nullptr || world.npcId(npc)>=world.npcCount())
      throw std::runtime_error("npcfight-record participant is not in the world");
    const auto id = npc->creationSeq();
    if(actors.size()>=4 || std::find(actors.begin(),actors.end(),id)!=actors.end())
      throw std::runtime_error("npcfight-record needs two to four distinct participants");
    actors.push_back(id);
    if(rows.size()>1)
      rows += ',';
    const auto* ranged = npc->currentRangedWeapon();
    const auto* symbol = ranged ? world.script().findSymbol(ranged->clsId()) : nullptr;
    rows += "{\"id\":"+std::to_string(id)+",\"npc_id\":"+std::to_string(world.npcId(npc))+
            ",\"position_cm\":"+ragdollVector(npc->position())+",\"mana\":"+std::to_string(npc->attribute(ATR_MANA))+
            ",\"ranged_weapon\":"+jsonStr(symbol ? symbol->name() : std::string_view{})+","+npcDuelSnapshot(world,*npc)+"}";
    };
  for(size_t i=3; i<args.size(); ++i) {
    const auto& selector = args[i];
    if(selector=="crew") {
      for(auto* npc:crew)
        add(npc);
      continue;
      }
    if(selector=="player" || selector.starts_with("id:")) {
      add(findNpc(world,player,selector));
      continue;
      }
    const auto symbol = world.script().findSymbolIndex(selector);
    Npc* npc = nullptr;
    for(uint32_t j=0; symbol!=size_t(-1) && j<world.npcCount(); ++j)
      if(auto* found=world.npcById(j); found!=nullptr && found->instanceSymbol()==symbol) {
        if(npc!=nullptr)
          throw std::runtime_error("npcfight-record ambiguous instance; use id:N or crew");
        npc = found;
        }
    add(npc);
    }
  if(actors.size()<2)
    throw std::runtime_error("npcfight-record needs two to four distinct participants");
  trace.actors = std::move(actors);
  trace.label = args[2];
  trace.from = now;
  event("npc_fight_record_begin","\"label\":"+jsonStr(trace.label)+",\"world\":"+jsonStr(world.name())+
        ",\"seed\":"+std::to_string(seed)+",\"t_game_ms\":"+std::to_string(now)+",\"participants\":"+rows+"]");
  Gothic::inst().onCombatHit.bind(&trace,&NpcFightTrace::hit);
  }

namespace {
using Combat = NpcCombatHandoff;
using Phase = Combat::Phase;
using Wait = Combat::Wait;
using Mode = Combat::Mode;
namespace FarCombat = Society::Combat;

bool nativeOwns(Phase phase) {
  return phase==Phase::Native || phase==Phase::NativeDrain || phase==Phase::Returned;
  }

bool ranged(const Npc& npc) {
  return npc.weaponState()==WeaponState::Bow || npc.weaponState()==WeaponState::CBow;
  }

const Combat::Actor* pendingMagicProjectile(const Combat& fight) {
  const auto& caster = fight.actors[0];
  return caster.nativeShots!=0 && fight.world->hasActiveProjectileFrom(*caster.npc) &&
         !fight.world->hasActiveProjectileFrom(*fight.actors[1].npc) ? &caster : nullptr;
  }

bool hasNativeWarmup(const Combat& fight) {
  return fight.actors[0].observedDamage>0 && (fight.mode==Mode::Magic || fight.actors[1].observedDamage>0);
  }

const Combat::Actor* pendingMeleeSwing(const Combat& fight) {
  const bool a = fight.actors[0].activeSwing!=0;
  const bool b = fight.actors[1].activeSwing!=0;
  if(a==b)
    return nullptr;
  const auto& attacker = fight.actors[a ? 0 : 1];
  const auto& defender = fight.actors[a ? 1 : 0];
  if(attacker.activeSwing==attacker.lastHitSwing || !attacker.npc->isPrehit() || defender.npc->isAttackAnim() ||
     attacker.npc->isMeleeChargeReleasing() || defender.npc->isMeleeDodging() || attacker.npc->target()!=defender.npc)
    return nullptr;
  return &attacker;
  }

void needHandoff(bool condition, const char* error) {
  if(!condition)
    throw std::runtime_error(std::string("npcfight-handoff: ")+error);
  }

std::string fightLedger(const Combat& fight) {
  const auto heroPosition = fight.world->player()->position();
  std::string fields = "\"tick\":"+std::to_string(fight.world->tickCount())+
                       ",\"actors\":[";
  for(const auto& a : fight.actors) {
    if(&a!=fight.actors.data())
      fields += ',';
    fields += "{\"actor\":"+std::to_string(a.id)+",\"ammo_start\":"+std::to_string(a.initialAmmo)+
              ",\"ammo\":"+std::to_string(a.ammo)+",\"hp_start\":"+std::to_string(a.initialHp)+
              ",\"hp\":"+std::to_string(a.hp)+",\"hero_distance_cm\":"+
              std::to_string(std::lround((a.npc->position()-heroPosition).length()))+
              ",\"native_shots\":"+std::to_string(a.nativeShots)+
              ",\"native_hits\":"+std::to_string(a.nativeHits);
    if(fight.mode==Mode::Magic)
      fields += ",\"mana_start\":"+std::to_string(a.initialMana)+",\"mana\":"+std::to_string(a.mana);
    fields += '}';
    }
  return fields+"],\"far_releases\":"+std::to_string(fight.releases)+
         ",\"far_impacts\":"+std::to_string(fight.impacts)+
         ",\"far_impacts_before_return\":"+std::to_string(fight.impactsWhileFar)+
         ",\"pending_at_return\":"+std::to_string(fight.pendingAtReturn)+
         ",\"native_drain_hits\":"+std::to_string(fight.nativeDrainHits);
  }

void startFarCombat(Combat& fight, FarCombat::Millis now) {
  // This one bench uses a deterministic exchange and each fighter's measured
  // HP loss from the native warmup. It is not a calibrated gameplay profile.
  std::array<FarCombat::Combatant,2> actors;
  std::array<FarCombat::AttackProfile,2> profiles;
  const auto deadline = now+15000;
  for(size_t i=0;i<actors.size();++i) {
    const auto& input = fight.actors[i];
    const bool magic = fight.mode==Mode::Magic;
    const bool recipient = magic && i==1;
    const bool weapon = magic ? input.npc->weaponState()==WeaponState::Mage :
                        fight.mode==Mode::Melee ? input.npc->weaponState()==WeaponState::W1H : ranged(*input.npc);
    needHandoff(!input.npc->hasMeleeWindow() && (recipient || (weapon && !input.npc->isInAir())),
                "the exchange requires two grounded actors with the chosen weapons");
    const auto p = input.npc->position();
    auto& actor = actors[i];
    actor.id = input.id;
    actor.side = uint32_t(i);
    actor.profile = uint32_t(i);
    actor.target = fight.actors[1-i].id;
    actor.position = {int32_t(std::lround(p.x)),int32_t(std::lround(p.z))};
    actor.hp = input.hp;
    actor.ammunition = int32_t(input.ammo);
    actor.mana = input.mana;
    actor.readyAt = recipient ? deadline : now+FarCombat::Millis(i)*100;

    auto& profile = profiles[i];
    profile.kind = magic ? FarCombat::AttackKind::Magic :
                   fight.mode==Mode::Melee ? FarCombat::AttackKind::HumanMelee : FarCombat::AttackKind::Ranged;
    profile.rangeCm = fight.mode==Mode::Melee ? 300 : 1800;
    profile.windup = 180;
    profile.recovery = 1050;
    profile.flight = fight.mode==Mode::Melee ? 0 : 700;
    profile.hitPermille = 1000;
    profile.damageMask = 1;
    profile.damage[0] = magic ? fight.actors[0].observedDamage : input.observedDamage;
    profile.resourceCost = magic ? fight.manaCost : (fight.mode==Mode::Melee ? 0 : 1);
    }
  fight.encounter.emplace(FarCombat::start(now,deadline,false,fight.seed,actors,profiles));
  fight.phase = Phase::Society;
  event("npc_combat_handoff_far",fightLedger(fight));
  }

void applyFarEvent(Combat& fight, const FarCombat::Event& step) {
  if(step.kind!=FarCombat::EventKind::Release && step.kind!=FarCombat::EventKind::Impact)
    return;
  auto& shooter = fight.actors[step.actor==fight.actors[0].id ? 0 : 1];
  auto& target  = fight.actors[step.target==fight.actors[0].id ? 0 : 1];
  uint32_t shot = shooter.pendingShot;
  const bool release = step.kind==FarCombat::EventKind::Release;
  const bool meleeRelease = release && fight.mode==Mode::Melee;
  needHandoff(shooter.id==step.actor && target.id==step.target && shooter.id!=target.id &&
              step.changes.size()==(meleeRelease ? 0 : 1), "unexpected far event");
  const auto change = meleeRelease ? FarCombat::Change{.id=shooter.id} : step.changes.front();
  if(release) {
    needHandoff(fight.phase==Phase::Society && shot==0 && change.id==shooter.id && change.hpDelta==0,"invalid release");
    if(fight.mode==Mode::Magic) {
      needHandoff(shooter.mana>=fight.manaCost && change.manaDelta==-fight.manaCost && change.ammunitionDelta==0,
                  "invalid mana debit");
      shooter.npc->changeAttribute(ATR_MANA,change.manaDelta,false);
      shooter.mana += change.manaDelta;
      } else if(!meleeRelease) {
      needHandoff(shooter.ammo>0 && change.ammunitionDelta==-1 && change.manaDelta==0,"invalid arrow debit");
      shooter.npc->delItem(shooter.ammunition,1);
      --shooter.ammo;
      }
    shooter.pendingShot = shot = ++fight.releases;
    }
  else {
    needHandoff(shot!=0 && change.id==target.id && change.hpDelta<0 && change.ammunitionDelta==0 &&
                change.manaDelta==0 && target.hp+change.hpDelta>1, "invalid, duplicate or lethal impact");
    target.npc->changeAttribute(ATR_HITPOINTS,change.hpDelta,false);
    target.hp += change.hpDelta;
    shooter.pendingShot = 0;
    ++fight.impacts;
    if(fight.phase==Phase::Society)
      ++fight.impactsWhileFar;
    }
  needHandoff((fight.mode!=Mode::Ranged || shooter.npc->itemCount(shooter.ammunition)==shooter.ammo) &&
              (fight.mode!=Mode::Magic || shooter.npc->attribute(ATR_MANA)==shooter.mana) &&
              target.npc->attribute(ATR_HITPOINTS)==target.hp, "live state did not apply the effect exactly");
  event("npc_combat_handoff_effect","\"shot\":"+std::to_string(shot)+
    ",\"operation\":"+jsonStr(release ? "release" : "impact")+
    ",\"controller\":"+jsonStr(fight.phase==Phase::Society ? "far" : "return_drain")+
    ",\"due\":"+std::to_string(step.at)+",\"shooter\":"+std::to_string(shooter.id)+
    ",\"target\":"+std::to_string(target.id)+",\"ammo_delta\":"+std::to_string(change.ammunitionDelta)+
    (fight.mode==Mode::Magic ? ",\"mana_delta\":"+std::to_string(change.manaDelta) : "")+
    ",\"hp_delta\":"+std::to_string(change.hpDelta)+","+fightLedger(fight));
  }
}

void NpcCombatHandoff::hit(Npc& victim, Npc& by, int32_t loss, std::string_view kind) {
  for(size_t i=0;i<actors.size();++i) {
    auto& target = actors[i];
    auto& shooter = actors[1-i];
    if(&victim!=target.npc)
      continue;
    const bool melee = mode==Mode::Melee;
    const bool magic = mode==Mode::Magic;
    if(&by!=shooter.npc || kind!=(magic ? "spell" : melee ? "melee" : "ranged") || !nativeOwns(phase) ||
       (magic && (i!=1 || victim.lastHitSpellId()!=spell || shooter.nativeHits>=shooter.nativeShots ||
                  (phase==Phase::NativeDrain && (shooter.id!=drainingActor || nativeDrainHits!=0)))) ||
       (melee && (shooter.activeSwing==0 || shooter.activeSwing==shooter.lastHitSwing ||
                  (phase==Phase::NativeDrain && (shooter.id!=drainingActor || shooter.activeSwing!=drainingSwing))))) {
      invalidNativeHit = true;
      return;
      }
    target.hp -= loss;
    if(shooter.observedDamage==0)
      shooter.observedDamage = loss;
    if(melee)
      shooter.lastHitSwing = shooter.activeSwing;
    ++shooter.nativeHits;
    if(phase==Phase::NativeDrain)
      ++nativeDrainHits;
    event("npc_combat_handoff_native_hit","\"shooter\":"+std::to_string(shooter.id)+
          ",\"target\":"+std::to_string(target.id)+",\"hp_delta\":"+std::to_string(-loss)+
          (melee ? ",\"swing\":"+std::to_string(shooter.activeSwing) : "")+
          (magic ? ",\"spell\":"+std::to_string(spell) : "")+
          ",\"draining\":"+(phase==Phase::NativeDrain ? "true" : "false")+","+fightLedger(*this));
    }
  }

void NpcCombatHandoff::stop() {
  Gothic::inst().onCombatHit.ubind(this,&NpcCombatHandoff::hit);
  *this = NpcCombatHandoff{};
  }

void npcCombatHandoffSwing(const Npc& npc, bool starts) {
  auto& fight = bench.npcCombatHandoff;
  if(fight.world!=&npc.world() || fight.mode!=Mode::Melee)
    return;
  for(auto& a : fight.actors) {
    if(a.npc!=&npc)
      continue;
    if(starts && ((fight.phase!=Phase::Native && fight.phase!=Phase::Returned) || a.activeSwing!=0)) {
      fight.invalidNativeHit = true;
      return;
      }
    if(starts)
      a.activeSwing = ++a.swings;
    else if(a.activeSwing==0)
      return;
    event(starts ? "npc_combat_handoff_native_swing_start" : "npc_combat_handoff_native_swing_end",
          "\"actor\":"+std::to_string(a.id)+",\"swing\":"+std::to_string(a.activeSwing)+
          ",\"draining\":"+(fight.phase==Phase::NativeDrain ? "true" : "false")+","+fightLedger(fight));
    if(!starts)
      a.activeSwing = 0;
    return;
    }
  }

BenchNpcTick npcCombatHandoffTick(const Npc& npc) {
  using Tick = BenchNpcTick;
  const auto& fight = bench.npcCombatHandoff;
  if(fight.world!=&npc.world() || (fight.actors[0].npc!=&npc && fight.actors[1].npc!=&npc))
    return Tick::Normal;
  if(fight.phase==Phase::NativeDrain)
    return fight.mode==Mode::Melee ? Tick::MeleeDrain : Tick::Paused;
  return fight.phase==Phase::Society || fight.phase==Phase::SocietyDrain ? Tick::Paused : Tick::Normal;
  }

void tickNpcCombatHandoff(World& world) {
  auto& fight = bench.npcCombatHandoff;
  if(fight.world==nullptr)
    return;
  needHandoff(fight.world==&world && !fight.invalidNativeHit, "world changed, unexpected native swing or hit");
  for(auto& actor : fight.actors) {
    needHandoff(world.npcById(world.npcId(actor.npc))==actor.npc && actor.npc->creationSeq()==actor.id &&
                !actor.npc->isDown() && actor.npc->attribute(ATR_HITPOINTS)==actor.hp,
                "participant changed or HP changed without an impact");
    const auto ammo = fight.mode==Mode::Ranged ? actor.npc->itemCount(actor.ammunition) : 0;
    if(ammo!=actor.ammo) {
      needHandoff((fight.phase==Phase::Native || fight.phase==Phase::Returned) && ammo+1==actor.ammo,
                  "unattributed ammo change");
      actor.ammo = ammo;
      ++actor.nativeShots;
      event("npc_combat_handoff_native_release","\"actor\":"+std::to_string(actor.id)+","+fightLedger(fight));
      }
    if(fight.mode==Mode::Magic) {
      const auto mana = actor.npc->attribute(ATR_MANA);
      if(mana!=actor.mana) {
        const auto* rune = actor.npc->activeWeapon();
        needHandoff(&actor==fight.actors.data() && (fight.phase==Phase::Native || fight.phase==Phase::Returned) && mana>=0 &&
                    actor.mana-mana==fight.manaCost && rune!=nullptr && rune->clsId()==fight.rune &&
                    rune->isRune() && rune->isSpellShoot() && world.hasActiveProjectileFrom(*actor.npc),
                    "mana changed without the selected native projectile release");
        if(fight.spell<0)
          fight.spell = rune->spellId();
        needHandoff(fight.spell==rune->spellId(),"native spell changed");
        actor.mana = mana;
        ++actor.nativeShots;
        event("npc_combat_handoff_native_release","\"actor\":"+std::to_string(actor.id)+
              ",\"mana_delta\":"+std::to_string(-fight.manaCost)+",\"spell\":"+std::to_string(fight.spell)+","+fightLedger(fight));
        }
      }
    }
  const auto* hero = world.player();
  needHandoff(hero!=nullptr,"missing observer");
  const bool far = std::ranges::all_of(fight.actors,[&](const Combat::Actor& a) {
    return (a.npc->position()-hero->position()).quadLength()>=6000.f*6000.f;
    });
  const auto now = FarCombat::Millis(world.tickCount());
  if(fight.phase==Phase::Native && far) {
    needHandoff(hasNativeWarmup(fight),"native damage sample missing before transfer");
    std::string fields;
    if(fight.mode==Mode::Melee) {
      const auto* a = pendingMeleeSwing(fight);
      needHandoff(a!=nullptr,"departure requires exactly one pending native melee swing");
      fight.drainingActor = a->id;
      fight.drainingSwing = a->activeSwing;
      fields = "\"draining_actor\":"+std::to_string(a->id)+",\"draining_swing\":"+std::to_string(a->activeSwing)+",";
      } else if(fight.mode==Mode::Magic) {
      const auto* a = pendingMagicProjectile(fight);
      needHandoff(a!=nullptr,"departure requires one caster with an active projectile");
      fight.drainingActor = a->id;
      fields = "\"draining_actor\":"+std::to_string(a->id)+",\"spell\":"+std::to_string(fight.spell)+",";
      }
    fight.phase = Phase::NativeDrain;
    event("npc_combat_handoff_native_drain",fields+fightLedger(fight));
    }
  if(fight.phase==Phase::NativeDrain) {
    needHandoff(far,"observer returned before the handoff");
    if(std::ranges::any_of(fight.actors,[&](const Combat::Actor& a) {
      if(fight.mode==Mode::Melee)
        return a.activeSwing!=0 || a.npc->isAttackAnim() || a.npc->hasMeleeWindow();
      return world.hasActiveProjectileFrom(*a.npc);
      }))
      return;
    if(fight.mode==Mode::Melee)
      needHandoff(fight.nativeDrainHits==1,"the departing melee swing did not land exactly once");
    else if(fight.mode==Mode::Magic)
      needHandoff(fight.nativeDrainHits==1,"the departing spell did not land exactly once");
    startFarCombat(fight,now);
    }
  if(fight.phase==Phase::Society && !far) {
    fight.pendingAtReturn = fight.releases-fight.impacts;
    needHandoff(fight.impactsWhileFar>0 && (fight.mode==Mode::Melee ||
                (fight.pendingAtReturn>0 && FarCombat::hasCommitted(*fight.encounter))),
                "return needs resolved far attacks and a committed shot for the ranged exchange");
    FarCombat::sealAttacks(*fight.encounter);
    fight.phase = Phase::SocietyDrain;
    event("npc_combat_handoff_return_drain",fightLedger(fight));
    }
  if(fight.phase!=Phase::Society && fight.phase!=Phase::SocietyDrain)
    return;
  while(fight.phase==Phase::Society || FarCombat::hasCommitted(*fight.encounter)) {
    const auto at = FarCombat::nextEventAt(*fight.encounter);
    if(!at || *at>now)
      break;
    const auto& step = FarCombat::step(*fight.encounter);
    needHandoff(step.outcome==FarCombat::Outcome::Active,"far encounter ended early");
    applyFarEvent(fight,step);
    }
  if(fight.phase==Phase::SocietyDrain && !FarCombat::hasCommitted(*fight.encounter)) {
    needHandoff(fight.releases==fight.impacts,"a committed shot was lost");
    for(auto& actor : fight.actors) {
      actor.hitsAtResume = actor.nativeHits;
      actor.shotsAtResume = actor.nativeShots;
      }
    fight.encounter.reset();
    fight.phase = Phase::Returned;
    event("npc_combat_handoff_native_resume",fightLedger(fight));
    }
  }

bool npcCombatHandoffWaiting(uint64_t tick) {
  auto& fight = bench.npcCombatHandoff;
  if(fight.wait==Wait::None)
    return false;
  bool ready = false;
  switch(fight.wait) {
    case Wait::NativeHit:
      ready = hasNativeWarmup(fight);
      break;
    case Wait::NativeSwing:
      ready = pendingMeleeSwing(fight)!=nullptr;
      break;
    case Wait::NativeProjectile:
      ready = pendingMagicProjectile(fight)!=nullptr;
      break;
    case Wait::FarRelease:
      ready = fight.impactsWhileFar>=2 && fight.releases>fight.impacts;
      break;
    case Wait::FarHits:
      ready = fight.impactsWhileFar>=2;
      break;
    case Wait::ReturnedHit:
      ready = fight.phase==Phase::Returned && std::ranges::any_of(fight.actors,[&](const Combat::Actor& a) {
        return a.nativeHits>a.hitsAtResume && (fight.mode==Mode::Melee || a.nativeShots>a.shotsAtResume);
        });
      break;
    case Wait::None:
      break;
    }
  if(ready)
    fight.wait = Wait::None;
  else
    needHandoff(tick<fight.waitUntil,"timed out waiting for the combat milestone");
  return !ready;
  }

void npcCombatHandoffCommand(const std::vector<std::string>& a, World& world, Npc& player, uint64_t seed) {
  auto& fight = bench.npcCombatHandoff;
  const bool magic = a.size()==6 && a[1]=="arm_magic";
  if(magic || (a.size()==4 && (a[1]=="arm" || a[1]=="arm_melee"))) {
    needHandoff(fight.phase==Phase::Off,"already armed");
    const bool melee = a[1]=="arm_melee";
    fight.mode = magic ? Mode::Magic : melee ? Mode::Melee : Mode::Ranged;
    if(melee)
      needHandoff(Settings::physicsFlags().meleePhysicsHits,"the melee fixture requires physical blade contacts");
    if(magic) {
      fight.rune = world.script().findSymbolIndex(a[4]);
      fight.manaCost = world.rules().intSymbol(a[5],0);
      needHandoff(fight.rune!=size_t(-1) && fight.manaCost>0,"expected a projectile rune and its mana cost");
      }
    for(size_t i=0;i<fight.actors.size();++i) {
      auto& actor = fight.actors[i];
      auto* npc = findNpc(world,player,a[2+i]);
      needHandoff(npc!=nullptr && npc!=&player && !npc->isImmortal() && !npc->isDown(),"expected two live NPCs");
      actor.npc = npc;
      actor.id = npc->creationSeq();
      actor.initialHp = actor.hp = npc->attribute(ATR_HITPOINTS);
      if(magic) {
        actor.initialMana = actor.mana = npc->attribute(ATR_MANA);
        needHandoff(npc->isHuman() && (i!=0 || actor.mana/fight.manaCost>=5),"expected two humans and sufficient caster mana");
        } else if(melee) {
        needHandoff(npc->isHuman() && npc->currentMeleeWeapon()!=nullptr && !npc->currentMeleeWeapon()->is2H() &&
                    !npc->isAttackAnim() && !npc->hasMeleeWindow(),"expected idle one-handed melee actors");
        } else {
        const auto* bow = npc->currentRangedWeapon();
        needHandoff(bow!=nullptr && bow->handle().munition>=0,"expected an archer");
        actor.ammunition = size_t(bow->handle().munition);
        actor.initialAmmo = actor.ammo = npc->itemCount(actor.ammunition);
        needHandoff(actor.ammo>=20 && actor.ammo<=1000,"expected a bounded supply of arrows");
        }
      }
    needHandoff(fight.actors[0].npc!=fight.actors[1].npc,"participants must be distinct");
    fight.world = &world;
    fight.seed = seed;
    fight.phase = Phase::Native;
    Gothic::inst().onCombatHit.bind(&fight,&Combat::hit);
    event("npc_combat_handoff_arm",fightLedger(fight));
    return;
    }
  needHandoff(fight.world==&world,"arm a duel first");
  if(a.size()==3 && a[1]=="wait") {
    if(a[2]=="native_hit")
      fight.wait = Wait::NativeHit;
    else if(a[2]=="native_swing" && fight.mode==Mode::Melee)
      fight.wait = Wait::NativeSwing;
    else if(a[2]=="native_projectile" && fight.mode==Mode::Magic)
      fight.wait = Wait::NativeProjectile;
    else if(a[2]=="far_hits" && fight.mode==Mode::Melee)
      fight.wait = Wait::FarHits;
    else if(a[2]=="far_release" && fight.mode!=Mode::Melee)
      fight.wait = Wait::FarRelease;
    else if(a[2]=="returned_hit")
      fight.wait = Wait::ReturnedHit;
    else
      throw std::runtime_error("npcfight-handoff: unknown milestone");
    fight.waitUntil = world.tickCount()+12000;
    return;
    }
  needHandoff(a.size()==2 && a[1]=="end" && fight.phase==Phase::Returned && fight.releases==fight.impacts &&
              (fight.mode==Mode::Melee || fight.pendingAtReturn>0) && (fight.mode==Mode::Ranged || fight.nativeDrainHits==1),
              "expected a complete round trip");
  event("npc_combat_handoff_end",fightLedger(fight));
  fight.stop();
  }

bool tickNpcDuel(NpcDuel& state,World& world) {
  auto& duel=state;
  if(!duel.active)return false;
  const uint64_t now=world.tickCount();
  const bool down[]={duel.actors[0]->isDown(),duel.actors[1]->isDown()};
  if(!down[0] && !down[1] && now<duel.end)return true;
  Gothic::inst().onCombatHit.ubind(&duel,&NpcDuel::hit);
  duel.active=false;
  std::string fields="\"seed\":"+std::to_string(duel.seed)+",\"duration_ms\":"+std::to_string(now-duel.from)+
    ",\"hit_log\":"+duel.hitLog+"],\"defender_first_swing_ms\":"+
    (duel.firstResponseMs<0 ? "null" : std::to_string(duel.firstResponseMs));
  if(down[0] || down[1]) {
    const size_t loser=duel.loser>=0 ? size_t(duel.loser) : (down[0] ? 0 : 1);
    fields+=",\"winner\":"+jsonStr(symName(world,duel.actors[1-loser]))+
            ",\"loser\":"+jsonStr(symName(world,duel.actors[loser]));
    } else fields+=",\"timeout\":true";
  std::vector<Item*> dropped;
  for(size_t i=0;i<2;++i) {
    Npc& npc=*duel.actors[i];
    bool weaponDropped=false;
    world.detectItem(npc.position(),3000.f,[&](Item& item) {
      if(item.handle().owner!=int32_t(npc.instanceSymbol()) || !item.isDynamic())return;
      if(item.clsId()==duel.weapons[i])weaponDropped=true;
      dropped.push_back(&item);
      });
    fields+=std::string(i==0 ? ",\"attacker\":{" : ",\"defender\":{")+duel.before[i]+
      ",\"hp_after\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
      ",\"hits\":"+std::to_string(duel.hits[i])+",\"damage_dealt\":"+std::to_string(duel.damage[i])+
      ",\"swings\":"+std::to_string(duel.swings[i])+",\"blocked\":"+std::to_string(duel.blocked[i])+
      ",\"misses\":"+std::to_string(duel.misses[i])+
      ",\"hp_one\":"+(npc.attribute(ATR_HITPOINTS)==1 ? "true" : "false")+
      ",\"unconscious\":"+(stateName(world,npc)=="ZS_UNCONSCIOUS" ? "true" : "false")+
      ",\"weapon_dropped\":"+(weaponDropped ? "true" : "false")+"}";
    }
  event("npc_duel",fields);
  for(auto* item:dropped)world.removeItem(*item);
  for(auto*& npc:duel.actors) {world.removeNpc(*npc);npc=nullptr;}
  return false;
  }

namespace {

std::vector<const WayPoint*> npcTierPath(const Npc& npc) {
  std::vector<const WayPoint*> result;
  if(const auto* current=npc.currentGoToPoint())result.push_back(current);
  auto path=npc.navigationPath();
  while(const auto* point=path.pop())
    if(result.empty() || result.back()!=point)result.push_back(point);
  return result;
  }

}

bool npcTierTestTick(NpcTierTest& state,World& world,Npc& player,uint64_t frameMs) {
  auto& test=state;
  if(!test.npc)return false;
  auto& npc=*test.npc;
  const auto now=world.tickCount();
  const bool turning=test.phase==1;
  const float moved=(npc.position()-test.lastPosition).length();
  const float turned=std::abs(std::remainder(npc.rotation()-test.lastAngle,360.f));
  test.lastPosition=npc.position();test.lastAngle=npc.rotation();
  if(now>=test.deadline) {
    test.npc=nullptr;
    arenaAbort("NPC tier transition test timed out before every action arrived");
    return true;
    }
  if(test.pending) {
    if(now==test.switchedAt)return true;
    const auto position=npc.position();
    const auto delta=position-test.before;
    const float horizontal=std::sqrt(delta.x*delta.x+delta.z*delta.z);
    const auto elapsed=now-test.switchedAt;
    const float travel=std::max(test.travelPerMs*float(elapsed),npc.animMoveSpeed(elapsed).length());
    const auto path=npcTierPath(npc);
    const bool suffix=path.size()<=test.path.size() && path.size()+1>=test.path.size() &&
      std::equal(path.rbegin(),path.rend(),test.path.rbegin());
    const auto goal=npc.navigationGoal();
    const bool sameGoal=turning || (goal.active && goal.point==test.destination);
    const auto* head=npc.aiQueueFront();
    const bool sameTurn=!turning || (head && head->act==AI_TurnToNpc);
    const float afterHeight=std::hypot(horizontal,std::max(0.f,std::abs(delta.y)-30.f));
    const bool pass=npc.simTier()!=test.previous && suffix && sameGoal && sameTurn && afterHeight<=travel+0.1f;
    test.maximumJump=std::max(test.maximumJump,delta.length());
    test.maximumHeight=std::max(test.maximumHeight,std::abs(delta.y));
    if(npc.simTier()==NpcSimTier::Full)++test.toFull[turning?1:0];
    else ++test.toSimplified[turning?1:0];
    event("npc_tier_transition","\"npc\":"+jsonStr(symName(world,&npc))+
      ",\"action\":"+jsonStr(turning?"turn":"edge")+",\"from\":"+jsonStr(NpcSimTiers::name(test.previous))+
      ",\"to\":"+jsonStr(NpcSimTiers::name(npc.simTier()))+",\"jump_cm\":"+ragdollNumber(delta.length())+
      ",\"horizontal_cm\":"+ragdollNumber(horizontal)+",\"height_cm\":"+ragdollNumber(std::abs(delta.y))+
      ",\"tick_travel_cm\":"+ragdollNumber(travel)+",\"after_height_correction_cm\":"+ragdollNumber(afterHeight)+",\"angle_delta_deg\":"+
      ragdollNumber(std::abs(std::remainder(npc.rotation()-test.beforeAngle,360.f)))+
      ",\"path_preserved\":"+(suffix?"true":"false")+",\"destination_preserved\":"+(sameGoal?"true":"false")+
      ",\"pass\":"+(pass?"true":"false"));
    test.pending=false;
    if(!pass) {
      test.npc=nullptr;
      arenaAbort("NPC tier transition changed the remaining action or exceeded one tick of travel");
      return true;
      }
    ++test.switches;
    }
  if(test.switches<2) {
    const auto* head=npc.aiQueueFront();
    const auto goal=npc.navigationGoal();
    const auto* current=npc.currentGoToPoint();
    const auto fromStart=npc.position()-test.started;
    const auto toWaypoint=current ? current->groundPos-npc.position() : Vec3();
    const bool ready=turning ? head && head->act==AI_TurnToNpc && turned>0.5f :
      current && goal.active && moved>0.1f && std::hypot(fromStart.x,fromStart.z)>50.f &&
      std::hypot(toWaypoint.x,toWaypoint.z)>200.f;
    if(!ready)return true;
    test.before=npc.position();
    test.beforeAngle=npc.rotation();
    test.destination=goal.point;
    test.path=npcTierPath(npc);
    test.previous=npc.simTier();
    const auto tick=std::max<uint64_t>(frameMs,1);
    test.travelPerMs=turning?0.f:std::max(npc.simplifiedMoveSpeed(),npc.animMoveSpeed(tick).length()/float(tick));
    test.switchedAt=now;
    test.pending=true;
    npc.setSimTierOverride(test.previous==NpcSimTier::Full?NpcSimTier::Simplified:NpcSimTier::Full);
    return true;
    }
  if(!npc.isAiQueueEmpty())return true;
  if(!turning) {
    const auto* destination=test.ends[(test.legs+1)%2];
    const bool arrived=(npc.position()-destination->position()).length()<=150.f;
    event("npc_tier_arrival","\"leg\":"+std::to_string(test.legs)+",\"destination\":"+jsonStr(destination->name)+
      ",\"distance_cm\":"+ragdollNumber((npc.position()-destination->position()).length())+
      ",\"pass\":"+(arrived?"true":"false"));
    if(!arrived) {test.npc=nullptr;arenaAbort("NPC tier transition action finished away from its destination");return true;}
    ++test.legs;
    if(test.legs<20) {
      test.started=npc.position();test.switches=0;
      npc.aiPush(AiQueue::aiGoToPoint(*test.ends[(test.legs+1)%2]));
      return true;
      }
    test.phase=1;
    }
  else ++test.turns;
  if(test.turns<20) {
    npc.setDirection(player.position()-npc.position());
    npc.setDirection(npc.rotation()+170.f);
    test.beforeAngle=npc.rotation();test.lastAngle=npc.rotation();test.switches=0;
    npc.aiPush(AiQueue::aiTurnToNpc(&player));
    return true;
    }
  const bool enough=test.toFull[0]>=20 && test.toSimplified[0]>=20 &&
                    test.toFull[1]>=20 && test.toSimplified[1]>=20 && test.legs==20 && test.turns==20;
  event("npc_tier_transition_result","\"edge_to_full\":"+std::to_string(test.toFull[0])+
    ",\"edge_to_simplified\":"+std::to_string(test.toSimplified[0])+
    ",\"turn_to_full\":"+std::to_string(test.toFull[1])+
    ",\"turn_to_simplified\":"+std::to_string(test.toSimplified[1])+
    ",\"arrivals\":"+std::to_string(test.legs)+",\"turns_completed\":"+std::to_string(test.turns)+
    ",\"max_jump_cm\":"+ragdollNumber(test.maximumJump)+",\"max_height_cm\":"+ragdollNumber(test.maximumHeight)+
    ",\"pass\":"+(enough?"true":"false"));
  npc.setSimTierOverride(std::nullopt);
  test.npc=nullptr;
  if(!enough)arenaAbort("NPC tier transition test did not observe 20 switches each way during both actions");
  return false;
  }

bool npcSpeedPrepare(NpcSpeedTest& state,World& world) {
  auto& test=state;
  static constexpr const char* instances[]={"BAU_904_BAUER","OrcWarrior_Roam","Wolf"};
  static constexpr WalkBit modes[]={WalkBit::WM_Walk,WalkBit::WM_Run,WalkBit::WM_Sneak};
  if(test.leg%6==0) {
    if(test.npc)world.removeNpc(*test.npc);
    const auto instance=world.script().findSymbolIndex(instances[test.leg/6]);
    test.npc=instance==size_t(-1) ? nullptr : world.addNpc(instance,test.start->groundPos);
    if(!test.npc || !test.npc->visualSkeleton() || test.npc->isDown()) {
      test.npc=nullptr;arenaAbort("NPC speed fixture instance is unavailable");return false;
      }
    }
  auto& npc=*test.npc;
  npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(true);npc.clearGoTo();
  npc.setTarget(nullptr);npc.setExternalControl(true);npc.stopWalking();npc.closeWeapon(true);npc.clearSpeed();
  npc.setSimTierOverride(test.leg%2==0 ? NpcSimTier::Full : NpcSimTier::Simplified);
  npc.setPosition(test.start->groundPos);npc.setDirection(test.end->groundPos-test.start->groundPos);
  npc.attachToPoint(test.start);npc.updateTransform();
  npc.aiPush(AiQueue::aiSetWalkMode(modes[(test.leg/2)%3]));
  npc.aiPush(AiQueue::aiGoToPoint(*test.end));
  test.began=test.previousTick=world.tickCount();test.previous=npc.position();
  test.measuredMs=0;test.samples=0;test.distance=0;test.expectedDistance=0;
  test.firstAnimation.clear();test.lastAnimation.clear();
  return true;
  }

bool npcSpeedTick(NpcSpeedTest& state,World& world) {
  auto& test=state;
  if(!test.npc)return false;
  auto& npc=*test.npc;
  const auto now=world.tickCount();
  if(now-test.began>600000) {
    test.npc=nullptr;arenaAbort("NPC speed fixture did not arrive on the authored edge");return true;
    }
  const auto edge=test.end->groundPos-test.start->groundPos;
  const auto offset=npc.position()-test.start->groundPos,previous=test.previous-test.start->groundPos;
  const auto progress=[&](const Vec3& p) {return (p.x*edge.x+p.z*edge.z)/(test.length*test.length);};
  if(now>test.previousTick && progress(previous)>=.2f && progress(offset)<=.8f) {
    const auto expected=test.leg%2==0 ? NpcSimTier::Full : NpcSimTier::Simplified;
    if(npc.simTier()!=expected || npc.currentGoToPoint()!=test.end) {
      test.npc=nullptr;arenaAbort("NPC speed fixture left the requested tier or authored edge");return true;
      }
    const auto dt=now-test.previousTick;
    const auto moved=npc.position()-test.previous;
    test.distance+=(moved.x*edge.x+moved.z*edge.z)/test.length;
    test.expectedDistance+=npc.simplifiedMoveSpeed()*float(dt);
    test.measuredMs+=dt;++test.samples;
    if(test.firstAnimation.empty())test.firstAnimation=npc.animNames();
    test.lastAnimation=npc.animNames();
    }
  test.previous=npc.position();test.previousTick=now;
  if(!npc.isAiQueueEmpty())return true;
  static constexpr const char* species[]={"human","orc","wolf"};
  static constexpr const char* modes[]={"walk","run","sneak"};
  const bool arrived=(npc.position()-test.end->groundPos).length()<=150.f;
  const bool pass=arrived && test.samples>=3 && test.distance>0;
  event("npc_speed_sample","\"species\":"+jsonStr(species[test.leg/6])+
    ",\"npc\":"+jsonStr(symName(world,&npc))+",\"walk_mode\":"+jsonStr(modes[(test.leg/2)%3])+
    ",\"tier\":"+jsonStr(NpcSimTiers::name(npc.simTier()))+
    ",\"from\":"+jsonStr(test.start->name)+",\"to\":"+jsonStr(test.end->name)+
    ",\"edge_cm\":"+ragdollNumber(test.length)+",\"edge_height_cm\":"+ragdollNumber(edge.y)+
    ",\"elapsed_ms\":"+std::to_string(now-test.began)+",\"measured_ms\":"+std::to_string(test.measuredMs)+
    ",\"samples\":"+std::to_string(test.samples)+",\"distance_cm\":"+ragdollNumber(test.distance)+
    ",\"sequence_speed_m_s\":"+ragdollNumber(test.measuredMs ? test.expectedDistance*10.f/float(test.measuredMs) : 0.f)+
    ",\"observed_speed_m_s\":"+ragdollNumber(test.measuredMs ? test.distance*10.f/float(test.measuredMs) : 0.f)+
    ",\"first_animation\":"+jsonStr(test.firstAnimation)+",\"last_animation\":"+jsonStr(test.lastAnimation)+
    ",\"arrived\":"+(arrived?"true":"false")+",\"pass\":"+(pass?"true":"false"));
  if(!pass) {test.npc=nullptr;arenaAbort("NPC speed fixture has no valid middle-edge measurement or arrival");return true;}
  if(++test.leg==18) {
    world.removeNpc(npc);test.npc=nullptr;
    event("npc_speed_result","\"rows\":18,\"species\":3,\"walk_modes\":3,\"tiers\":2,\"pass\":true");
    return false;
    }
  npcSpeedPrepare(state,world);
  return true;
  }

bool npcSimCommand(NpcDuel& duelState,NpcTierTest& tierState,NpcSpeedTest& speedState,size_t& pc,const std::vector<std::string>& prog,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="npctier") {
    // npctier <npc> full|simplified|auto: a runtime-only override of one actor.
    Npc* npc=a.size()==3?findNpc(w,pl,a[1]):nullptr;
    if(!npc || (a[2]!="full" && a[2]!="simplified" && a[2]!="auto")) {
      arenaAbort("npctier expects an existing NPC and full, simplified or auto");
      return true;
      }
    npc->setSimTierOverride(a[2]=="auto"?std::nullopt:
      std::optional<NpcSimTier>(a[2]=="full"?NpcSimTier::Full:NpcSimTier::Simplified));
    event("npc_tier_override","\"npc\":"+jsonStr(symName(w,npc))+",\"mode\":"+jsonStr(a[2]));
    }
  else if(c=="npctier-test") {
    // npctier-test <npc> <start waypoint> <end waypoint>: 20 walking legs and
    // 20 turns, each with one switch in either direction while still active.
    Npc* npc=a.size()==4?findNpc(w,pl,a[1]):nullptr;
    const auto* start=a.size()==4?w.findPoint(a[2],false):nullptr;
    const auto* end=a.size()==4?w.findPoint(a[3],false):nullptr;
    if(!npc || npc==&pl || !start || !end || start==end) {
      arenaAbort("npctier-test expects an NPC and two distinct existing waypoints");
      return true;
      }
    tierState=NpcTierTest();
    auto& test=tierState;
    test.npc=npc;test.ends[0]=start;test.ends[1]=end;
    test.deadline=w.tickCount()+1800000;
    npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);
    npc->setExternalControl(true);npc->stopWalking();npc->closeWeapon(true);
    npc->setSimTierOverride(NpcSimTier::Full);npc->setWalkMode(WalkBit::WM_Walk);
    const auto floor=w.physic()->landRay(start->position()+Vec3(0,100,0),300.f);
    if(!floor.hasCol) {test.npc=nullptr;arenaAbort("npctier-test start waypoint has no ground");return true;}
    npc->setPosition(floor.v);npc->updateTransform();
    test.started=npc->position();test.lastPosition=npc->position();test.lastAngle=npc->rotation();
    npc->aiPush(AiQueue::aiGoToPoint(*end));
    event("npc_tier_transition_start","\"npc\":"+jsonStr(symName(w,npc))+",\"from\":"+jsonStr(start->name)+
      ",\"to\":"+jsonStr(end->name));
    }
  else if(c=="npcspeed") {
    const auto* start=a.size()==2?w.findPoint(a[1],false):nullptr;
    const WayPoint* end=nullptr;
    float length=0;
    if(start)for(const auto& connection:start->connections()) {
      const auto edge=connection.point->groundPos-start->groundPos;
      const float distance=std::hypot(edge.x,edge.z);
      if(distance>length) {end=connection.point;length=distance;}
      }
    if(!start || !end || length<500.f) {
      arenaAbort("npcspeed expects an authored waypoint with a connected edge at least 5 m long");return true;
      }
    // This fresh calibration world has no ambient combat, crowding or other
    // actors using the edge; the player's distant position is retained.
    for(uint32_t i=w.npcCount();i>0;--i)
      if(auto* npc=w.npcById(i-1);npc!=&pl)w.removeNpc(*npc);
    speedState=NpcSpeedTest();
    auto& test=speedState;test.start=start;test.end=end;test.length=length;
    npcSpeedPrepare(speedState,w);
    }
  else if(c=="npcduel") {
    auto fail=[&](const std::string& reason) {
      say("npcduel: "+reason);event("npc_duel_error","\"reason\":"+jsonStr(reason));
      pc=prog.size();
      };
    if(a.size()<4 || a.size()>5) {fail("expected attacker defender seed [timeout_ms]");return true;}
    const auto first=w.script().findSymbolIndex(a[1]), second=w.script().findSymbolIndex(a[2]);
    const auto attack=w.script().findSymbolIndex("ZS_ATTACK");
    if(first==size_t(-1) || second==size_t(-1) || attack==size_t(-1)) {fail("missing script instance or ZS_ATTACK");return true;}
    const float an=pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an)), side(-forward.z,0,forward.x);
    const Vec3 center=pl.position()+forward*1200.f;
    Vec3 pos[]={center-side*150.f,center+side*150.f};
    for(auto& p:pos) {
      DynamicWorld::RayLandResult ground;
      for(float y=p.y+1500;y>p.y-1500 && !ground.hasCol;y-=400)
        ground=w.physic()->ray({p.x,y,p.z},{p.x,y-450,p.z});
      if(!ground.hasCol) {fail("arena ground not found");return true;}
      p.y=ground.v.y;
      }
    auto& duel=duelState;
    duel=NpcDuel{};
    duel.seed=uint32_t(std::stoul(a[3]));
    const uint64_t timeout=a.size()>4 ? std::stoull(a[4]) : 180000;
    w.script().setSeed(duel.seed);
    duel.actors[0]=w.addNpc(first,pos[0]);duel.actors[1]=w.addNpc(second,pos[1]);
    for(size_t i=0;i<2;++i) {
      auto* npc=duel.actors[i];
      if(npc==nullptr || npc->isMonster() || npc->handle().guild>=16) {
        for(auto*& copy:duel.actors)if(copy) {w.removeNpc(*copy);copy=nullptr;}
        fail("two human NPCs with guild < 16 are required");return true;
        }
      duel.before[i]=npcDuelSnapshot(w,*npc);
      if(auto* weapon=npc->currentMeleeWeapon())duel.weapons[i]=weapon->clsId();
      auto& flags=npc->handle().flags;
      flags=daedalus::NpcFlag(uint32_t(flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
      npc->setDirection(pos[1-i]-pos[i]);npc->updateTransform();
      npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
      }
    pl.handle().flags=daedalus::NpcFlag(uint32_t(pl.handle().flags)|uint32_t(daedalus::NpcFlag::IMMORTAL));
    pl.clearAiQueue();pl.clearGoTo();pl.clearSpeed();
    Npc& npc=*duel.actors[0];Npc& target=*duel.actors[1];
    // Same nonlethal start as npc_act fight{deed:true}; the defender retains its constructor state.
    auto& aiv=npc.handle().aivar;
    aiv[size_t(HeroDeed::constant(npc,"AIV_ATTACKREASON"))]=HeroDeed::constant(npc,"AR_NONE");
    aiv[size_t(HeroDeed::constant(npc,"AIV_LASTFIGHTAGAINSTPLAYER"))]=HeroDeed::constant(npc,"FIGHT_CANCEL",0);
    aiv[size_t(HeroDeed::constant(npc,"AIV_LASTFIGHTCOMMENT"))]=0;
    npc.stopListening();npc.thinkOfAnswer(0);npc.clearAiQueue();
    if(!npc.startState(ScriptFn(attack),"",gtime::endOfTime(),true)) {
      for(auto*& copy:duel.actors) {w.removeNpc(*copy);copy=nullptr;}
      fail("cannot start ZS_ATTACK");return true;
      }
    npc.setOther(&target);npc.setVictim(nullptr);npc.setTarget(&target);
    npc.setNonlethalFight(target);npc.setServerDeed("attack",&target);
    if(npc.weaponState()==WeaponState::NoWeapon)npc.aiPush(AiQueue::aiDrawWeapon());
    duel.from=w.tickCount();duel.end=duel.from+timeout;duel.active=true;
    Gothic::inst().onCombatHit.bind(&duel,&NpcDuel::hit);
    }
  else return false;
  return true;
  }

}
