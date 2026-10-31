#include "damagecalculator.h"

#include <cmath>

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/bullet.h"
#include "game/archery.h"
#include "gothic.h"
#include "testbench.h"

// https://forum.worldofplayers.de/forum/threads/127320-Damage-System?p=2198181#post2198181
// https://strafkolonie-online.net/forum/board/thread/1895-info-erkl%C3%A4rung-der-berechnung-der-trefferchance-im-fernkampf/

static float mix(float x, float y, float a) {
  return x + (y-x)*a;
  }

DamageCalculator::Val DamageCalculator::damageValue(Npc& src, Npc& other, const Bullet* b, bool isSpell, const DamageCalculator::Damage& splDmg, const CollideMask bMsk) {
  DamageCalculator::Val ret;
  if(b!=nullptr) {
    ret = rangeDamage(src,other,*b,bMsk);
    } else
  if(isSpell) {
    ret = rangeDamage(src,other,splDmg,bMsk);
    }
  else {
    ret = swordDamage(src,other);
    }

#if 0
  // debug
  ret.value = MinDamage;
#endif

  if(ret.hasHit && !ret.invincible && Gothic::inst().version().game==2)
    ret.value = std::max<int32_t>(ret.value,MinDamage);
  return ret;
  }

float DamageCalculator::meleeMultiplier(float speedCmS, float typicalCmS, float bladeFraction, float surfaceAngleDeg, size_t targetIndex) {
  const float speed = speedCmS/std::max(typicalCmS,1.f);
  const float lever = .5f+.5f*std::clamp(bladeFraction,0.f,1.f);
  const float edge = .5f+.5f*std::sin(std::clamp(surfaceAngleDeg,0.f,90.f)*float(M_PI)/180.f);
  return std::clamp(speed*lever*edge,.5f,1.5f)*std::pow(.6f,float(targetIndex));
  }

int32_t DamageCalculator::vanillaChance(const GameScript& script, int32_t hitChance, float dist) {
  // the talent's chance at RANGED_CHANCE_MINDIST, all the way up close; past it from two thirds of it
  // down to none at RANGED_CHANCE_MAXDIST: the line H - H*d/max, it steps there (the exe; the formula the
  // forums give runs on without the step)
  const float mn = std::max(script.rangedChanceMinDist(),1.f);
  const float mx = std::max(script.rangedChanceMaxDist(),mn);
  const float h  = float(hitChance);
  const float c  = dist<=mn ? 100.f + (h-100.f)*dist/mn : h - h*dist/mx;
  return std::max(0,int32_t(c));
  }

DamageCalculator::Val DamageCalculator::damageFall(Npc& npc, float speed) {
  auto  gl = npc.guild();
  auto& g  = npc.world().script().guildVal();

  float   gravity     = DynamicWorld::gravity;
  float   fallTime    = speed/gravity;
  float   height      = 0.5f*std::abs(gravity)*fallTime*fallTime;
  float   h0          = float(g.falldown_height[gl]);
  float   dmgPerMeter = float(g.falldown_damage[gl]);
  int32_t prot        = npc.protection(::PROT_FALL);

  Val ret;
  ret.invincible = (prot<0);
  ret.value      = int32_t(dmgPerMeter*(height-h0)/100.f - float(prot));
  if(ret.value<=0 || ret.invincible) {
    ret.value = 0;
    return ret;
    }
  ret.hasHit = true;
  return ret;
  }

DamageCalculator::Val DamageCalculator::rangeDamage(Npc& nsrc, Npc& nother, const Bullet& b, const CollideMask bMsk) {
  float dist       = b.pathLength();
  bool  noHit      = dist>float(MaxMagRange);
  bool  invincible = !checkDamageMask(nsrc,nother,&b);
  auto  dmg        = b.damage();

  float zone = 1;
  if(!b.isSpell() && b.isPrecise()) {
    // an arrow aimed by hand hit where it hit, no dice: the draw makes the arrow, the armour takes its part,
    // the zone then scales what went through (a head shot does twice the harm, not the weapon twice)
    const float k = Archery::damageAt(b.draw());
    for(auto& v:dmg.val)
      v = int32_t(std::lround(float(v)*k));
    zone  = Archery::zoneDamage(b.hitZone());
    noHit = false;
    }
  else if(!b.isSpell() && Gothic::inst().version().game==2) {
    // G2's dice: one throw, the chance by the distance between the two (not the arrow's way)
    auto&         script = nsrc.world().script();
    const float   d      = (nother.position()-nsrc.position()).length();
    const int32_t talent = int32_t(std::lround(b.hitChance()*100.f));
    const int32_t chance = vanillaChance(script,talent,d);
    const int32_t roll   = int32_t(script.rand(100,&nsrc));
    noHit = roll>=chance;
    if(TestBench::isActive()) {
      const auto who = TestBench::who(&nother);
      const auto by  = TestBench::who(&nsrc);
      char buf[256];
      std::snprintf(buf,sizeof(buf),"arrow dice %.*s: dist %.0f path %.0f talent %d chance %d roll %d, %s, by %.*s",
                    int(who.size()),who.data(),double(d),double(dist),int(talent),int(chance),int(roll),noHit ? "miss" : "hit",
                    int(by.size()),by.data());
      TestBench::note(buf);
      }
    }
  else if(!b.isSpell()) {
    auto& script    = nsrc.world().script();
    float hitChance = float(script.rand(100,&nsrc))/100.f;
    float hitCh     = 0;
    bool  g2        = Gothic::inst().version().game==2;
    float refRange  = g2 ? ReferenceBowRangeG2 : ReferenceBowRangeG1;
    float maxRange  = float(MaxBowRange);
    float chance    = b.hitChance();

    if(dist<refRange)
      hitCh = mix(1.f, chance, (dist / refRange));
    else if(dist<maxRange)
      hitCh = mix(chance, 0.f, (dist-refRange) / (maxRange-refRange));
    else
      hitCh = 0;

    noHit = (dist>float(MaxBowRange) || hitCh<=hitChance);
    if(TestBench::isActive()) {
      const auto who = TestBench::who(&nother);
      const auto by  = TestBench::who(&nsrc);
      char buf[256];
      std::snprintf(buf,sizeof(buf),"arrow dice %.*s: dist %.0f path %.0f talent %.0f chance %.1f roll %.0f, %s, by %.*s",
                    int(who.size()),who.data(),double((nother.position()-nsrc.position()).length()),double(dist),
                    double(chance*100.f),double(hitCh*100.f),double(hitChance*100.f),noHit ? "miss" : "hit",
                    int(by.size()),by.data());
      TestBench::note(buf);
      }

    if(!g2 && !noHit && !invincible) {
      const int32_t mul        = script.criticalDamageMultiplyer();
      const int     critChance = int(script.rand(100,&nsrc));
      if(std::lround(100.f * b.critChance())>critChance)
        dmg *= mul;
      }
    }

  if(noHit)
    return Val(0,false,invincible);

  if(invincible)
    return Val(0,true,true);

  if((bMsk & (COLL_APPLYDAMAGE | COLL_APPLYDOUBLEDAMAGE | COLL_APPLYHALVEDAMAGE | COLL_DOEVERYTHING))==0)
    return Val(0,true,true);

  auto ret = rangeDamage(nsrc,nother,dmg,bMsk);
  if(zone!=1.f)
    ret.value = int32_t(std::lround(float(ret.value)*zone));
  return ret;
  }

DamageCalculator::Val DamageCalculator::rangeDamage(Npc&, Npc& nother, Damage dmg, const CollideMask bMsk) {
  auto& other = nother.handle();

  if(bMsk & COLL_APPLYDOUBLEDAMAGE)
    dmg*=2;
  if(bMsk & COLL_APPLYHALVEDAMAGE)
    dmg/=2;

  int  value = 0;
  bool invincible = true;
  for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
    if(dmg[size_t(i)]==0)
      continue;
    int vd = std::max(dmg[size_t(i)] - other.protection[i],0);
    if(other.protection[i]>=0) { // Filter immune
      value     += vd;
      invincible = false;
      }
    }

  return Val(value,true,invincible);
  }

DamageCalculator::Val DamageCalculator::swordDamage(Npc& nsrc, Npc& nother) {
  if(!checkDamageMask(nsrc,nother,nullptr))
    return Val(0,true,true);

  auto& script = nsrc.world().script();
  auto& src    = nsrc.handle();
  auto& other  = nother.handle();

  // Swords/Fists
  const int dtype      = damageTypeMask(nsrc);
  Talent    tal        = TALENT_UNKNOWN;
  int       str        = nsrc.attribute(Attribute::ATR_STRENGTH);
  int       critChance = int(script.rand(100,&nsrc));

  int value = 0;

  if(auto w = nsrc.inventory().activeWeapon()) {
    if(w->is2H())
      tal = TALENT_2H; else
      tal = TALENT_1H;
    }

  if(Gothic::inst().version().game==2) {
    if(nsrc.isMonster() && tal==TALENT_UNKNOWN) {
      // regular monsters always do critical damage
      critChance = -1;
      }

    bool invincible = true;
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
      if((dtype & (1<<i))==0)
        continue;
      int vd = std::max(str + src.damage[i] - other.protection[i],0);
      if(src.hitchance[tal]<=critChance)
        vd = (vd-1)/10;
      if(other.protection[i]>=0) { // Filter immune
        value += vd;
        invincible = false;
        }
      }

    return Val(value,true,invincible);
    } else {
    bool invincible = true;
    const int32_t mul = script.criticalDamageMultiplyer();
    for(unsigned int i=0; i<zenkit::DamageType::NUM; ++i) {
      if((dtype & (1<<i))==0)
        continue;
      int vd = 0;
      if(nsrc.talentValue(tal)<=critChance)
        vd = std::max(str +     src.damage[i] - other.protection[i],0); else
        vd = std::max(str + mul*src.damage[i] - other.protection[i],0);
      if(other.protection[i]>=0) { // Filter immune
        value += vd;
        invincible = false;
        }
      }

    return Val(value,true,invincible);
    }
  }

int32_t DamageCalculator::damageTypeMask(Npc& npc) {
  if(auto w = npc.inventory().activeWeapon())
    return w->handle().damage_type;
  return npc.handle().damage_type;
  }

bool DamageCalculator::checkDamageMask(Npc& nsrc, Npc& nother, const Bullet* b) {
  auto& other = nother.handle();

  if(b!=nullptr) {
    auto dmg = b->damage();
    for(unsigned int i=0;i<zenkit::DamageType::NUM;++i) {
      if(dmg[size_t(i)]>0 && other.protection[i]>=0)
        return true;
      }
    } else {
    const int dtype = damageTypeMask(nsrc);
    for(unsigned int i=0;i<zenkit::DamageType::NUM;++i){
      if((dtype & (1<<i))==0)
        continue;
      return true;
      }
    }

  return false;
  }

DamageCalculator::Damage DamageCalculator::rangeDamageValue(Npc& src) {
  const int dtype = damageTypeMask(src);
  int d = Gothic::inst().version().game==2 ? src.attribute(Attribute::ATR_DEXTERITY) : 0;
  Damage ret={};
  for(unsigned int i=0;i<zenkit::DamageType::NUM;++i){
    if((dtype & (1<<i))==0)
      continue;
    ret[size_t(i)] = d + src.handle().damage[i];
    }
  return ret;
  }
