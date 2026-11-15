#include "npc.h"

#include "game/bumpsignal.h"
#include "world/objects/sound.h"
#include "world/world.h"

#include <algorithm>

using namespace Tempest;

namespace {

Flinch::Bump::Kind bumpKind(BumpHit::Kind kind) {
  switch(kind) {
    case BumpHit::Kind::Wall:   return Flinch::Bump::Wall;
    case BumpHit::Kind::Waist:  return Flinch::Bump::Waist;
    case BumpHit::Kind::Person: return Flinch::Bump::Person;
    case BumpHit::Kind::Pushed: return Flinch::Bump::Pushed;
    }
  return Flinch::Bump::Wall;
  }

}

// A run into an obstacle (game/bumpsignal): the upper body answers, the root and the controls do not
void Npc::bumpReaction(const BumpHit& hit) {
  Flinch::Bump bump;
  bump.kind     = bumpKind(hit.kind);
  bump.normal   = hit.normal;
  bump.strength = hit.strength;

  // near the hero the living rig takes the push on its motors too, as it takes a blow: the arms sway
  const Vec3 thrown = hit.kind==BumpHit::Kind::Waist ? hit.normal*-1.f : hit.normal;
  queueBumpChain(thrown,hit.strength,hit.kind==BumpHit::Kind::Waist ? 0.45f : 0.75f);
  visual.bump(*this,bump);

  // one dull thud, as loud as the bump is hard
  const Vec3 feet = position();
  auto sound = ::Sound(owner,::Sound::T_Regular,"M_FALL_SMALL",Vec3(feet.x,feet.y+120.f,feet.z),1500.f,false);
  sound.setVolume(sound.volume()*(0.35f+0.65f*std::clamp(hit.strength,0.f,1.f)));
  sound.play();
  }

bool Npc::queueBumpChain(const Vec3& thrown, float strength, float height) {
  if(!living || !isHuman())
    return false;
  syncDeferredAnimation(); // the bone is chosen by the pose
  const Vec3 dir = Vec3(thrown.x,0,thrown.z);
  if(dir.quadLength()<=0.f || strength<=0.f)
    return false;
  const Vec3  d    = Vec3::normalize(dir);
  const Vec3  feet = position();
  const float top  = std::max(mapHeadBone().y - feet.y, 50.f);
  // the side of the body the push comes from, at the chest or at the hips
  Vec3 at = collosionCenter() - d*20.f;
  at.y = feet.y + top*height;
  const auto& transform = visual.transform();
  const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
  const size_t bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,at);
  if(bone>=visual.pose().boneCount())
    return false;
  // a body's push is broad and slow beside a blade's: a small impulse, the motors keep most of their hold
  const float s = std::clamp(strength,0.f,1.f);
  arrowImpacts.push_back({bone,at,d*(2.f+4.f*s),0.3f+0.25f*s,{},false,2.f});
  return true;
  }
