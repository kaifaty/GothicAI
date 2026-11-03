#include "game/meleeriposte.h"
#include "game/meleestamina.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  void requireNear(float actual, float expected, const char* message) {
    if(std::isfinite(actual) && std::abs(actual-expected)<.0001f)
      return;
    std::fprintf(stderr,"FAIL: %s: actual %.9g, expected %.9g\n",message,double(actual),double(expected));
    std::exit(EXIT_FAILURE);
    }

  void requirePoint(MeleeRiposte::Point actual, MeleeRiposte::Point expected, const char* message) {
    requireNear(actual.x,expected.x,message);
    requireNear(actual.y,expected.y,message);
    requireNear(actual.z,expected.z,message);
    }
  }

int main() {
  using namespace MeleeRiposte;

  Situation stance;
  stance.player = stance.standingOneHand = stance.heroClip = stance.victimHuman = stance.victimAlive =
      stance.victimStunned = stance.victimClip = true;
  stance.distanceCm = 150.f;
  require(available(stance),"a stunned human in front of a standing 1H hero allows a riposte");
  for(auto member:{&Situation::enabled,&Situation::player,&Situation::standingOneHand,&Situation::heroClip,
                   &Situation::victimHuman,&Situation::victimAlive,&Situation::victimStunned,&Situation::victimClip}) {
    auto s = stance;
    s.*member = false;
    require(!available(s),"disabled, NPC, 2H/fist, dead, unstunned, nonhuman or missing clip prevents a riposte");
    }
  auto s = stance;
  s.victimDown = true;
  require(!available(s),"a prone or knocked down victim cannot be riposted");
  auto ai = stance;
  ai.player = false;
  require(!available(ai),"AI without melee_ai_riposte cannot riposte");
  ai.aiEnabled = true;
  require(available(ai),"melee_ai_riposte admits AI in the same standing 1H situation");
  for(const auto& actor:{stance,ai}) {
    for(float distance:{79.f,80.f,250.f,251.f}) {
      s = actor;
      s.distanceCm = distance;
      require(available(s)==(distance>=80.f && distance<=250.f),"hero and AI distance includes 80 and 250 cm only");
      }
    for(float height:{50.f,51.f,-50.f,-51.f}) {
      s = actor;
      s.heightCm = height;
      require(available(s)==(std::abs(height)<=50.f),"hero and AI height includes 50 cm in either direction");
      }
    for(float angle:{45.f,46.f,-45.f,-46.f}) {
      s = actor;
      s.heroAngleDeg = angle;
      require(available(s)==(std::abs(angle)<=45.f),"hero and AI facing includes 45 degrees in either direction");
      }
    for(float angle:{119.f,120.f,-119.f,-120.f}) {
      s = actor;
      s.victimAngleDeg = angle;
      require(available(s)==(std::abs(angle)<120.f),"hero and AI 120 degrees follows the existing backstab path");
      }
    }

  const Point hero{10.f,20.f,30.f};
  const auto pair = pairPosition(hero,.6f,.8f);
  requirePoint(pair,{109.f,20.f,162.f},"pair is 165 cm along the hero's starting facing direction");
  const Point victim{209.f,40.f,262.f};
  requirePoint(alignment(victim,pair,0),victim,"alignment begins at the victim's root");
  requirePoint(alignment(victim,pair,100),{193.375f,40.f,246.375f},"horizontal alignment uses smoothstep and preserves the victim's height");
  requirePoint(alignment(victim,pair,200),{159.f,40.f,212.f},"horizontal alignment is halfway at 200 ms without changing height");
  requirePoint(alignment(victim,pair,400),{109.f,40.f,162.f},"alignment reaches the horizontal pair at 400 ms");
  requirePoint(alignment(victim,pair,500),{109.f,40.f,162.f},"alignment remains at the horizontal pair after 400 ms");

  const auto bladeAt25 = frameTimeMs(10,25.f);
  const auto bladeAt50 = frameTimeMs(10,50.f);
  const auto shiftedBlade = frameTimeMs(11,25.f);
  require(bladeAt25==400 && bladeAt50==200 && shiftedBlade==440,"DEF_OPT_FRAME timing follows the clip's frame and fps");
  require(!bladeReady(399,bladeAt25) && bladeReady(400,bladeAt25),"blade enters at frame 10 of a 25 fps clip");
  require(!bladeReady(199,bladeAt50) && bladeReady(200,bladeAt50),"blade enters at frame 10 of a 50 fps clip");
  require(!bladeReady(439,shiftedBlade) && bladeReady(440,shiftedBlade),"blade timing follows a changed DEF_OPT_FRAME");
  require(!handoverReady(839) && handoverReady(840),"handover occurs at 840 ms, extraction frame 21");
  const auto riposte = damageMultipliers(multiplier(),1.5f,3.f);
  requireNear(riposte.riposte,3.f,"riposte damage multiplier is 3");
  requireNear(riposte.stun,1.f,"riposte replaces the stunned 1.5 multiplier");
  requireNear(riposte.backstab,1.f,"riposte does not stack with a backstab multiplier");
  const auto ordinary = damageMultipliers(1.f,1.5f,1.f);
  requireNear(ordinary.riposte,1.f,"ordinary attacks have no riposte multiplier");
  requireNear(ordinary.stun,1.5f,"ordinary attacks keep their stunned multiplier");
  requireNear(ordinary.backstab,1.f,"ordinary attacks keep their backstab selection");
  const auto backstab = damageMultipliers(1.f,1.f,3.f);
  requireNear(backstab.backstab,3.f,"ordinary backstabs keep their multiplier");
  require(superArmor(true,0,1920) && superArmor(true,1919,1920) && superArmor(true,1920,1920),
          "super armor covers attack start through DEF_HIT_END");
  require(!superArmor(true,1921,1920) && !superArmor(false,0,1920),"super armor ends after DEF_HIT_END");

  const auto ordinaryCost = MeleeStamina::attackCost(40,60);
  requireNear(cost(ordinaryCost,true),ordinaryCost,"riposte costs one ordinary weapon attack");
  requireNear(cost(ordinaryCost,false),0.f,"without melee_stamina the riposte is free");

  std::puts("melee riposte contract passed");
  }
