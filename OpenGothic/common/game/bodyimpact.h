#pragma once

namespace BodyImpact {

enum class Perception { None, Defeat, Murder };

struct Decision final {
  bool knockOut = false;
  bool captureMurder = false;
  Perception perception = Perception::None;
  };

constexpr Decision decide(bool nonlethalPair, bool humanVictim, bool playerVictim, bool hasCulprit, bool reachesOneHp) {
  if(!hasCulprit || !reachesOneHp)
    return {};
  if(nonlethalPair && humanVictim && !playerVictim)
    return {true,false,Perception::Defeat};
  // Murder still requires death: an unprotected impact may leave exactly one HP.
  return {false,true,Perception::Murder};
  }

}
