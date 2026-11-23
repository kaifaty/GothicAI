#include "quiverhand.h"

#include <cmath>
#include <string_view>

#include "ikmath.h"
#include "skeleton.h"

using namespace Tempest;
using namespace IkMath;

void QuiverHand::setSkeleton(const Skeleton* sk) {
  *this = QuiverHand();
  if(sk==nullptr)
    return;
  auto& nodes = sk->nodes;
  auto  find  = [&](std::string_view name) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(nodes[i].name==name)
        return i;
    return size_t(-1);
    };
  auto  isUnder = [&](size_t id, size_t root) {
    for(size_t i=id; i<nodes.size(); i=nodes[i].parent)
      if(i==root)
        return true;
    return false;
    };
  auto  tree = [&](size_t root, std::vector<size_t>& out) {
    for(size_t i=0; i<nodes.size(); ++i)
      if(isUnder(i,root))
        out.push_back(i);
    };
  pelvis = find("BIP01 PELVIS");
  upper  = find("BIP01 R UPPERARM");
  fore   = find("BIP01 R FOREARM");
  hand   = find("BIP01 R HAND");
  finger = find("BIP01 R FINGER0");
  if(pelvis==size_t(-1) || upper==size_t(-1) || fore==size_t(-1) || hand==size_t(-1))
    return;
  if(!isUnder(fore,upper) || !isUnder(hand,fore))
    return;
  tree(upper,upperTree);
  tree(fore,foreTree);
  tree(hand,handTree);
  valid = true;
  }

bool QuiverHand::update(const Input& i) {
  const bool was = in.weight>0.f;
  in = i;
  if(!(in.weight>0.f))
    missNow = -1;
  return was || in.weight>0.f;
  }

void QuiverHand::apply(Matrix4x4* tr, size_t numBones) {
  if(!valid || !(in.weight>0.f) || pelvis>=numBones || upper>=numBones || fore>=numBones || hand>=numBones)
    return;
  const float w    = std::min(in.weight,1.f);
  const Vec3  grip = position(tr[pelvis]*in.grip);
  // the wrist so the palm is at the grip, the hand turned as the animation has it
  Vec3 fingers = finger<numBones ? position(tr[finger])-position(tr[hand]) : Vec3();
  if(length(fingers)<1e-3f)
    fingers = Vec3(0,-1,0);
  fingers = normalize(fingers);
  const Vec3 from = position(tr[hand]);
  const Vec3 to   = from + (grip-fingers*Palm - from)*w;
  // the elbow leans as the animation has it, away from the arm's middle line (not the upper arm's way: a target far
  // from the hand turns that through the shoulder-wrist line and the elbow flips)
  const Vec3 sh   = position(tr[upper]);
  const Vec3 bend = position(tr[fore]) - (sh+from)*0.5f;
  TwoBone tb;
  if(!twoBone(tr,upper,fore,hand,upperTree,foreTree,to,bend,Reach,0.f,tb))
    return;
  rotateAbout(tr,handTree,transpose(mul(tb.r1,tb.r0)),position(tr[hand]));
  missNow = length(position(tr[hand])+fingers*Palm-grip);
  }
