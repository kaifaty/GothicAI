#include "game/bodyimpact.h"

#include <array>
#include <cstdio>
#include <cstdlib>

int main() {
  using BodyImpact::Decision;
  using BodyImpact::Perception;
  // Bits: nonlethal pair, human victim, player victim, culprit present, HP would reach <= 1.
  constexpr Decision none{};
  constexpr Decision murder{false,true,Perception::Murder};
  constexpr Decision defeat{true,false,Perception::Defeat};
  constexpr std::array<Decision,32> expected = {
    // No nonlethal pair.
    none,none,none,murder, none,none,none,murder,
    none,none,none,murder, none,none,none,murder,
    // A nonhuman in a nonlethal pair still dies with murder attribution.
    none,none,none,murder, none,none,none,murder,
    // Only the non-player human can be knocked out.
    none,none,none,defeat, none,none,none,murder,
    };
  for(size_t inputs=0;inputs<expected.size();++inputs) {
    const auto actual=BodyImpact::decide((inputs&16)!=0,(inputs&8)!=0,(inputs&4)!=0,(inputs&2)!=0,(inputs&1)!=0);
    const auto& want=expected[inputs];
    if(actual.knockOut!=want.knockOut || actual.captureMurder!=want.captureMurder || actual.perception!=want.perception) {
      std::fprintf(stderr,"FAIL: body impact inputs %zu\n",inputs);
      return EXIT_FAILURE;
      }
    }
  std::puts("body impact contract passed: 32 input combinations");
  }
