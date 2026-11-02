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
  for(float distance:{79.f,80.f,250.f,251.f}) {
    s = stance;
    s.distanceCm = distance;
    require(available(s)==(distance>=80.f && distance<=250.f),"distance includes 80 and 250 cm only");
    }
  for(float height:{50.f,51.f,-50.f,-51.f}) {
    s = stance;
    s.heightCm = height;
    require(available(s)==(std::abs(height)<=50.f),"height includes 50 cm in either direction");
    }
  for(float angle:{45.f,46.f,-45.f,-46.f}) {
    s = stance;
    s.heroAngleDeg = angle;
    require(available(s)==(std::abs(angle)<=45.f),"hero facing includes 45 degrees in either direction");
    }
  for(float angle:{119.f,120.f,-119.f,-120.f}) {
    s = stance;
    s.victimAngleDeg = angle;
    require(available(s)==(std::abs(angle)<120.f),"120 degrees follows the existing backstab path");
    }

  const Point hero{10.f,20.f,30.f};
  const auto pair = pairPosition(hero,.6f,.8f);
  requirePoint(pair,{109.f,20.f,162.f},"pair is 165 cm along the hero's starting facing direction");
  const Point victim{209.f,40.f,262.f};
  requirePoint(alignment(victim,pair,0),victim,"alignment begins at the victim's root");
  requirePoint(alignment(victim,pair,100),{193.375f,36.875f,246.375f},"alignment uses smoothstep");
  requirePoint(alignment(victim,pair,200),{159.f,30.f,212.f},"alignment is halfway at 200 ms");
  requirePoint(alignment(victim,pair,400),pair,"alignment reaches the pair at 400 ms");
  requirePoint(alignment(victim,pair,500),pair,"alignment remains at the pair after 400 ms");

  require(!bladeReady(399,400) && bladeReady(400,400),"blade enters at the clip's DEF_OPT_FRAME");
  require(!bladeReady(439,440) && bladeReady(440,440),"blade timing follows supplied animation metadata");
  require(!handoverReady(839) && handoverReady(840),"handover occurs at 840 ms, extraction frame 21");
  requireNear(multiplier(),3.f,"riposte replaces the stunned 1.5 multiplier with 3");
  require(superArmor(true,0,1920) && superArmor(true,1919,1920) && superArmor(true,1920,1920),
          "super armor covers attack start through DEF_HIT_END");
  require(!superArmor(true,1921,1920) && !superArmor(false,0,1920),"super armor ends after DEF_HIT_END");

  const auto ordinaryCost = MeleeStamina::attackCost(40,60);
  requireNear(cost(ordinaryCost,true),ordinaryCost,"riposte costs one ordinary weapon attack");
  requireNear(cost(ordinaryCost,false),0.f,"without melee_stamina the riposte is free");

  std::puts("melee riposte contract passed");
  }
