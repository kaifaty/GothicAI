#include "npc.h"

void Npc::bumpReaction(const BumpHit& hit) {
  Flinch::Hit reaction;
  reaction.dir = hit.kind==BumpHit::Kind::Waist ? hit.normal*-1.f : hit.normal;
  reaction.height = hit.kind==BumpHit::Kind::Waist ? 0.3f : 0.75f;
  reaction.twist = 0.f;
  reaction.strength = 0.5f*hit.strength;
  flinch(reaction);
  }
