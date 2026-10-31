#include "game/meleeparry.h"

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
  }

int main() {
  using namespace MeleeParry;
  requireNear(windowMs(0),120.f,"novice parry window");
  requireNear(windowMs(50),185.f,"half-trained parry window");
  requireNear(windowMs(100),250.f,"master parry window");
  requireNear(windowMs(-10),120.f,"talent lower clamp");
  requireNear(windowMs(110),250.f,"talent upper clamp");

  require(classify(119,0,480)==Result::Parry,"119 ms parries at zero talent");
  require(classify(120,0,480)==Result::Guard,"120 ms starts novice guard");
  require(classify(121,0,480)==Result::Guard,"121 ms guards at zero talent");
  require(classify(184,50,480)==Result::Parry,"184 ms parries at talent 50");
  require(classify(185,50,480)==Result::Guard,"185 ms starts half-trained guard");
  require(classify(249,100,480)==Result::Parry,"249 ms parries at talent 100");
  require(classify(250,100,480)==Result::Guard,"250 ms starts master guard");
  require(classify(251,100,480)==Result::Guard,"251 ms guards at talent 100");
  for(const int talent : {0,50,100}) {
    require(classify(479,talent,480)==Result::Guard,"479 ms remains inside defence");
    require(classify(480,talent,480)==Result::None,"480 ms closes defence");
    require(classify(481,talent,480)==Result::None,"481 ms is outside defence");
    }

  requireNear(guardCost(0.f),5.f,"guard cost lower clamp");
  requireNear(guardCost(5.f),5.f,"guard cost lower boundary");
  requireNear(guardCost(25.f),25.f,"guard cost follows posture increase");
  requireNear(guardCost(50.f),50.f,"guard cost upper boundary");
  requireNear(guardCost(100.f),50.f,"guard cost upper clamp");
  require(guardBroken(24.f,25.f,true),"stamina shortage breaks guard");
  require(!guardBroken(25.f,25.f,true),"exact stamina price holds guard");
  require(!guardBroken(26.f,25.f,true),"surplus stamina holds guard");
  require(!guardBroken(0.f,25.f,false),"disabled stamina never breaks guard");
  std::puts("melee parry contract passed");
  }
