#include "bowdraw.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "ikmath.h"
#include "aimframe.h"
#include "skeleton.h"
#include "quiver.h"

using namespace Tempest;
using namespace IkMath;

namespace {

constexpr float Brace    = 17.f;  // cm: the string at rest from the grip (a bow without a morph)
constexpr float MaxSpan  = 80.f;  // cm: a longer draw is not a bow held in the aim (the pose is off)
constexpr float Reach    = 0.98f; // of the arm's length: it does not straighten further...
constexpr float Comfort  = 0.92f; // ...nor reaches for the string further: a string out of reach rests
                                  // drawn a bit, the draw starts from there
constexpr float BlendIn  = 0.15f; // seconds: the hand goes to the string...
constexpr float BlendOut = 0.15f; // ...and back to the animation
constexpr float HoldIn   = 0.3f;  // seconds: the bow goes up in the hand, to the arrow (eased)
constexpr float LetDown  = 3.f;   // 1/s: the string let down without a shot goes back that fast
constexpr float MinShot  = 0.3f;  // drawn that far the string let go is a shot (Archery::MinDraw)
constexpr float HeadR    = 10.f;  // cm: the head with the hair, a capsule from the neck to the crown that thick
constexpr float RoundOut = 15.f;  // cm from the head's axis at least: the hand goes round it, on its side
constexpr float NockBack = 36.f;  // cm the nock is behind the slot the arrow sits in (G2)...
constexpr float TipAhead = 40.f;  // ...the tip ahead of it
constexpr float Shelf    = 10.f;  // cm the bow goes up in the hand: the arrow at its middle lies on the
                                  // hand (the top of G2's fist is 7.5 cm over the slot the bow is in)
constexpr float Follow   = 4.f;   // cm the hand flies back past the face as the string goes...
constexpr float FollowT  = BowDraw::FollowTime; // ...for that long, there and back (smooth at both ends)

// the release as an archer's (reports/archery/2026-11-03-release-follow-through): times from the shot
constexpr float Snap      = 3.f;   // cm the fingers fly back off the string at once...
constexpr float SnapT     = 0.06f; // ...in that many seconds
constexpr float BackMin   = 3.f;   // cm on back along the neck toward the ear, a light bow drawn short...
constexpr float BackKick  = 6.f;   // ...and that much more for the hardest kick
constexpr float BackT     = 0.3f;  // seconds: there by then (eased out), held
constexpr float OutMin    = 1.5f;  // cm out from the head with the snap (the fingers go back along the cheek: not into it)...
constexpr float OutKick   = 1.f;
constexpr float FadeAt    = 0.6f;  // seconds: the hand let go of the follow through (if not gone on to the
constexpr float FadeT     = 0.3f;  // quiver), so slowly
constexpr float BowKickMin = 0.5f; // cm the bow hand is thrown back toward the archer...
constexpr float BowKickK   = 1.f;
constexpr float BowKickT   = 0.03f; // ...in that long (Klopsteg's photographs), back on the line after
constexpr float BowBackT   = 0.09f;
constexpr float TurnMin   = 2.f;   // degrees the bow turns in the closed hand, the top limb forward...
constexpr float TurnKick  = 6.f;
constexpr float TurnT     = 0.03f; // ...at once, upright again in TurnBack
constexpr float TurnBack  = 0.2f;
constexpr float ShakeDeg  = 1.5f;  // degrees: the bow's shake at the hardest kick...
constexpr float ShakeHz   = 16.f;  // ...as the limbs ring (14..18 Hz)...
constexpr float ShakeTau  = 0.1f;  // ...dying out (seconds, by the archer's strength: Archery::releaseSettle)
constexpr float RelT      = FadeAt+FadeT; // seconds the release goes on
constexpr float HeadIn    = 1.5f;  // cm: the follow through takes the hand no deeper into the head (the capsule
                                   // of the hair) than that, or than the pose had it

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_BOWDRAW");
  return v==nullptr || v[0]!='0';
  }();

bool quiverReload = []() {
  const char* v = std::getenv("OPENGOTHIC_QUIVERRELOAD");
  return v==nullptr || v[0]!='0';
  }();

// the trunk for the reload's bench: an upright elliptic cylinder round the spine from the pelvis to the chest's
// top (cm, half its width and depth, the naked body; reports/archery/2026-11-05-quiver-reload)
// the arrow's mesh (ITRW_ARROW, 94 cm): its nock and tip from the slot it sits in (the reload's bench; the follow
// through keeps G2's NockBack, TipAhead)
constexpr float ArrowNock = 23.f;
constexpr float ArrowTip  = 71.f;
constexpr float TrunkWide = 15.f;
constexpr float TrunkDeep = 11.f;
constexpr float TrunkTop  = 15.f; // cm up the spine over BIP01 SPINE2 (the shoulders are over it: not counted)

bool releaseOn = []() {
  const char* v = std::getenv("OPENGOTHIC_BOWRELEASE");
  return v==nullptr || v[0]!='0';
  }();

float easeOut(float x) {
  x = std::clamp(x,0.f,1.f);
  const float y = 1.f-x;
  return 1.f-y*y*y;
  }

float smooth(float x) {
  x = std::clamp(x,0.f,1.f);
  return x*x*(3.f-2.f*x);
  }

// From a to b round an axis (c, unit u), turning through e1's side of it (e1, e2: across the axis): the
// angle about the axis, the height along it and the distance from it go evenly; the distance is not less
// than r on the way
Vec3 roundAxis(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& u, const Vec3& e1, const Vec3& e2,
               float r, float s) {
  auto polar = [&](const Vec3& p, float& h, float& d, float& an) {
    const Vec3 rel = p-c;
    h  = dot(rel,u);
    const Vec3 rho = rel-u*h;
    d  = length(rho);
    an = std::atan2(dot(rho,e2),dot(rho,e1));
    };
  float ha, da, aa, hb, db, ab;
  polar(a,ha,da,aa);
  polar(b,hb,db,ab);
  const float h  = ha + (hb-ha)*s;
  const float an = aa + (ab-aa)*s;
  float       d  = da + (db-da)*s;
  // away from the axis on the way (not at the ends: it starts at the cheek)
  d = d + std::max(0.f,r-d)*std::sin(float(M_PI)*s);
  return c + u*h + e1*(d*std::cos(an)) + e2*(d*std::sin(an));
  }

// cm between segment a-b and segment c-d
float segDist(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
  // sampled: short segments, a few cm is enough for the bench
  float best = 1e9f;
  for(int i=0; i<=16; ++i) {
    const Vec3  p  = a + (b-a)*(float(i)/16.f);
    const Vec3  cd = d-c;
    const float l  = dot(cd,cd);
    const float t  = l>0.f ? std::clamp(dot(p-c,cd)/l,0.f,1.f) : 0.f;
    best = std::min(best,length(c+cd*t-p));
    }
  return best;
  }

// a direction in a bone's space to the world
Vec3 turnOf(const Matrix4x4& m, const Vec3& d) {
  return Vec3(m.at(0,0)*d.x + m.at(1,0)*d.y + m.at(2,0)*d.z,
              m.at(0,1)*d.x + m.at(1,1)*d.y + m.at(2,1)*d.z,
              m.at(0,2)*d.x + m.at(1,2)*d.y + m.at(2,2)*d.z);
  }

Vec3 arrowAxis(const Matrix4x4& slot) {
  return normalize(Vec3(-slot.at(0,0),-slot.at(0,1),-slot.at(0,2)));
  }

}

BowDraw::Probe BowDraw::playerProbe;

bool BowDraw::isEnabled() {
  return enabled;
  }

void BowDraw::setEnabled(bool e) {
  enabled = e;
  Log::i("bowdraw: ", e ? "on" : "off");
  }

bool BowDraw::isQuiverReload() {
  return quiverReload;
  }

bool BowDraw::isReleaseEnabled() {
  return releaseOn;
  }

void BowDraw::setReleaseEnabled(bool e) {
  releaseOn = e;
  Log::i("bowrelease: ", e ? "on" : "off");
  }

float BowDraw::looseIntensity(float kick) {
  return releaseOn ? 0.4f + 0.6f*std::clamp(kick,0.f,1.f) : 1.f;
  }

void BowDraw::setSkeleton(const Skeleton* sk) {
  *this = BowDraw();
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

  upper   = find("BIP01 R UPPERARM");
  fore    = find("BIP01 R FOREARM");
  hand    = find("BIP01 R HAND");
  slot    = find("ZS_RIGHTHAND"); // the arrow sits there
  head    = find("BIP01 HEAD");
  crown   = find("ZS_HELMET");
  chest   = find("BIP01 SPINE2");
  neck    = find("BIP01 NECK");
  clav    = find("BIP01 R CLAVICLE");
  pelvis  = find("BIP01 PELVIS");
  bowHand = find("ZS_LEFTHAND");  // the bow
  if(bowHand==size_t(-1))
    bowHand = find("BIP01 L HAND");
  if(upper==size_t(-1) || fore==size_t(-1) || hand==size_t(-1) || slot==size_t(-1) || bowHand==size_t(-1))
    return;
  if(!isUnder(fore,upper) || !isUnder(hand,fore) || !isUnder(slot,hand))
    return;
  tree(upper,upperTree);
  tree(fore,foreTree);
  tree(hand,handTree);
  valid = true;

  // the bow arm: raises the bow to the arrow
  lUpper = find("BIP01 L UPPERARM");
  lFore  = find("BIP01 L FOREARM");
  lHand  = find("BIP01 L HAND");
  if(lUpper!=size_t(-1) && lFore!=size_t(-1) && lHand!=size_t(-1) &&
     isUnder(lFore,lUpper) && isUnder(lHand,lFore) && isUnder(bowHand,lHand)) {
    tree(lUpper,lUpperTree);
    tree(lFore,lForeTree);
    tree(lHand,lHandTree);
    }

  // the upper body to hold in the reload, the drawing arm in it; the hand goes round the head
  const size_t spine = find("BIP01 SPINE");
  if(spine==size_t(-1) || chest==size_t(-1) || head==size_t(-1) || crown==size_t(-1) ||
     !isUnder(hand,chest) || !isUnder(bowHand,spine) || !isUnder(chest,spine))
    return;
  bodyRoot = nodes[spine].parent;
  if(bodyRoot>=nodes.size())
    return;
  tree(spine,bodyTree);
  heldRel.resize(nodes.size());
  spine0 = spine;
  // the own reload's clip moves the drawing arm (from its clavicle on the neck) and the head
  if(neck!=size_t(-1) && clav!=size_t(-1) && pelvis!=size_t(-1) && nodes[clav].parent==neck &&
     isUnder(upper,clav) && isUnder(head,neck)) {
    tree(clav,armTree);
    tree(head,headTree);
    ownRel.resize(nodes.size());
    }
  }

bool BowDraw::grabNodes(size_t& c, size_t& h, size_t& e) const {
  if(bodyTree.empty())
    return false;
  c = chest;
  h = hand;
  e = fore;
  return true;
  }

bool BowDraw::update(const Input& in) {
  if(!valid)
    return false;
  const float dt   = float(in.dt)/1000.f;
  const bool  on   = in.active && in.ready && enabled;
  const float prev     = weight;
  const float prevDraw = draw;
  const float prevBack = follow;
  const float prevTurn = align;
  const float prevCant = cantK;
  active     = in.active;
  cantK      = std::clamp(in.cant,0.f,1.f);
  inHand     = in.arrow;
  given      = in.span;
  hasString  = in.hasString;
  stringRest = in.stringRest;
  stringPull = in.stringPull;
  stringHigh = in.stringHigh;
  bowAxis    = in.bowAxis;
  if(in.grabKnown) {
    grabKnown = true;
    grabHand  = in.grabHand;
    grabElbow = in.grabElbow;
    }
  own         = in.own && in.reload>=0.f && !armTree.empty();
  ownKnown    = own && in.ownKnown;
  ownHand0    = in.ownHand0;
  ownHead0    = in.ownHead0;
  quiverKnown = in.quiverKnown;
  quiverNow   = in.quiverNow;
  quiverClip  = in.quiverClip;
  reloadP     = in.reload;
  // the aim pose is taken while the bow is held still; the reload holds it
  capture  = in.active && in.ready && enabled && !bodyTree.empty();
  holding  = in.active && in.reload>=0.f && enabled && held && !bodyTree.empty();
  // the quiver reached at GrabAt, or later, quick hands no faster than MinReach
  const float grab = in.reloadTime>0.f ? std::clamp(MinReach/in.reloadTime,GrabAt,0.4f) : GrabAt;
  const float nock = std::max(NockAt,grab+0.3f);
  toQuiver = holding ? smooth(in.reload/grab) : 0.f;
  toString = holding ? smooth((in.reload-grab)/(nock-grab)) : 0.f;
  if(holding && own) {
    // the own clip: it reaches the quiver itself; the string as the aim has it from StringFrom
    toQuiver = smooth(in.reload/OwnGrab);
    toString = smooth((in.reload-StringFrom)/(StringAt-StringFrom));
    }
  // the shot: the string held drawn goes (not held still any more, not let down): the hand follows through
  if(in.active && wasReady && !in.ready && draw>=MinShot && weight>0.f) {
    followT   = 0;
    shotAt    = travel;
    kickK     = std::clamp(in.kick,0.f,1.f);
    settleK   = in.settle;
    }
  wasReady = in.active && in.ready;
  const float prevKick = bowKick, prevTurn2 = bowTurn, prevOut = relOut, prevFade = relFade;
  if(releaseOn) {
    // the hand flies back off the string and on toward the ear, does not come back: the reload takes it on
    // from there; the bow hand is thrown back and comes to the line again, the bow turns in it and rings
    followT = std::min(RelT, followT+dt);
    const float t = followT;
    const bool  on = t<RelT;
    relBack = on ? Snap*easeOut(t/SnapT) + (BackMin+BackKick*kickK)*easeOut((t-SnapT)/(BackT-SnapT)) : 0.f;
    relOut  = on ? (OutMin+OutKick*kickK)*easeOut(t/SnapT) : 0.f;
    relFade = on ? 1.f - smooth((t-FadeAt)/FadeT) : 0.f;
    follow  = relBack*relFade;
    const float kick = BowKickMin+BowKickK*kickK;
    bowKick = t<BowKickT ? kick*easeOut(t/BowKickT) : kick*(1.f-smooth((t-BowKickT)/BowBackT));
    const float tau   = ShakeTau*settleK;
    const float turn  = TurnMin+TurnKick*kickK;
    const float shape = t<TurnT ? easeOut(t/TurnT) : 1.f-smooth((t-TurnT)/TurnBack);
    const float shake = ShakeDeg*kickK*std::exp(-t/std::max(tau,1e-3f))*std::sin(2.f*float(M_PI)*ShakeHz*t);
    bowTurn = on ? turn*shape + shake : 0.f;
    } else {
    followT = std::min(FollowT, followT+dt);
    const float bump = std::sin(float(M_PI)*followT/FollowT);
    follow  = followT<FollowT ? Follow*bump*bump : 0.f;
    relBack = follow;
    relOut  = 0;
    relFade = 1;
    bowKick = 0;
    bowTurn = 0;
    }
  if(on) {
    weight = std::min(1.f, weight + dt/BlendIn);
    // drawn as it is; let down (no shot) back a bit slower
    draw   = in.draw>=draw ? in.draw : std::max(in.draw, draw - dt*LetDown);
    }
  else if(holding) {
    // the reload: the hand goes for the next arrow and brings it to the string (not drawn)
    weight = toString;
    draw   = 0;
    }
  else {
    // not holding still: the hand as it was goes back to the animation
    weight = std::max(0.f, weight - dt/BlendOut);
    }
  // the bow held as an archer holds it while it is held in the aim, the reload too
  if(on || holding)
    alignT = std::min(1.f, alignT + dt/HoldIn); else
    alignT = std::max(0.f, alignT - dt/BlendOut);
  align = smooth(alignT);
  return weight!=prev || (weight>0.f && draw!=prevDraw) || follow!=prevBack || holding || align!=prevTurn ||
         bowKick!=prevKick || bowTurn!=prevTurn2 || relOut!=prevOut || relFade!=prevFade ||
         (align>0.f && cantK!=prevCant);
  }

void BowDraw::hold(Matrix4x4* tr, size_t numBones, const Matrix4x4& object) {
  if(!valid || bodyTree.empty() || bodyTree.back()>=numBones || bodyRoot>=numBones)
    return;
  // the frame the upper body is held in: turned as the body faces, at the pelvis
  const Matrix4x4 frame = compose(rotationOf(object),position(tr[bodyRoot]));
  frameNow = frame;
  if(holding) {
    // the own clip: its drawing arm and head relative to its neck, and the quiver it takes the arrow from, before
    // the upper body is held
    ownHeld = false;
    if(own && !armTree.empty() && armTree.back()<numBones && headTree.back()<numBones && neck<numBones &&
       pelvis<numBones) {
      Matrix4x4 inv = tr[neck];
      inv.inverse();
      for(auto i:armTree)
        ownRel[i] = inv*tr[i];
      for(auto i:headTree)
        ownRel[i] = inv*tr[i];
      quiverOfNeck = inv*tr[pelvis]*quiverClip;
      ownHeld = true;
      }
    for(auto i:bodyTree)
      tr[i] = frame*heldRel[i];
    if(ownHeld) {
      Matrix4x4 inv = tr[neck];
      inv.inverse();
      clavHeld = inv*tr[clav];
      }
    return;
    }
  if(capture) {
    // the aim pose as animated
    Matrix4x4 inv = frame;
    inv.inverse();
    for(auto i:bodyTree)
      heldRel[i] = inv*tr[i];
    held = true;
    }
  }

void BowDraw::measureHead(const Matrix4x4* tr, size_t numBones) {
  gapHand = gapFore = gapArrow = 99;
  if(head>=numBones || hand>=numBones || fore>=numBones || slot>=numBones)
    return;
  const Vec3 h0 = position(tr[head]);
  const Vec3 h1 = crown<numBones ? position(tr[crown]) : h0+Vec3(0,20,0);
  const Vec3 ax = arrowAxis(tr[slot]);
  const Vec3 sl = position(tr[slot]);
  gapHand  = segDist(position(tr[hand]),position(tr[hand]),h0,h1) - HeadR;
  gapFore  = segDist(position(tr[fore])*0.5f+position(tr[hand])*0.5f,position(tr[hand]),h0,h1) - HeadR;
  gapArrow = inHand ? segDist(sl-ax*NockBack,sl+ax*TipAhead,h0,h1) - HeadR : 99.f;
  }

void BowDraw::apply(Matrix4x4* tr, size_t numBones) {
  implApply(tr,numBones);
  measureHead(tr,numBones);
  measureReload(tr,numBones);
  if(hand<numBones) {
    Matrix4x4 inv = frameNow;
    inv.inverse();
    handAt = position(inv*tr[hand]);
    }
  }

bool BowDraw::moveHand(Matrix4x4* tr, const Vec3& target, const Vec3* bendTo) {
  // the elbow bends in the plane it is animated in, or toward where it is asked to
  const Vec3 bend = bendTo!=nullptr ? *bendTo : position(tr[fore]) - position(tr[upper]);
  TwoBone    tb;
  if(!twoBone(tr,upper,fore,hand,upperTree,foreTree,target,bend,Reach,0.f,tb))
    return false;
  // the hand keeps its turn: the arrow keeps its line
  rotateAbout(tr,handTree,transpose(mul(tb.r1,tb.r0)),position(tr[hand]));
  return true;
  }

void BowDraw::stringNow(const Matrix4x4* tr, Vec3& rest, Vec3& pull) const {
  // drawn straight back as far as the bow is held so, the rest as G2 draws it
  rest = stringRest;
  tr[bowHand].project(rest);
  pull = turnOf(tr[bowHand],stringHigh + (stringPull-stringHigh)*align);
  }

void BowDraw::alignBow(Matrix4x4* tr, const Vec3& up, const Vec3& nock, const Vec3& axis, float lean) {
  // the bow slides up in the hand: the hand below its middle, the arrow on the hand...
  const Vec3 slide = up*(Shelf*align);
  slid = slide;
  tr[bowHand].set(3,0,tr[bowHand].at(3,0)+slide.x);
  tr[bowHand].set(3,1,tr[bowHand].at(3,1)+slide.y);
  tr[bowHand].set(3,2,tr[bowHand].at(3,2)+slide.z);
  // ...the bow hand raises it to the arrow: the string drawn straight back ends at the nock. Canted, the
  // bow turns round the arrow from there: the arrow stays on the hand, the string at the nock
  Vec3 rest, pull;
  stringNow(tr,rest,pull);
  const Vec3 from = position(tr[lHand]);
  // after the shot the bow hand is thrown back toward the archer a moment, along the arrow
  Vec3       to   = from + (nock-(rest+pull))*align - axis*bowKick;
  Mat3       turn = rotationOf(tr[lHand]);
  if(lean!=0.f) {
    const Mat3 r = axisAngle(axis,lean);
    to   = nock + mul(r,to-nock);
    turn = mul(r,turn);
    slid = mul(r,slid);
    }
  bowMove = length(to-from);
  if(bowMove<0.01f)
    return;
  TwoBone    tb;
  const Vec3 bend = position(tr[lFore]) - position(tr[lUpper]);
  if(!twoBone(tr,lUpper,lFore,lHand,lUpperTree,lForeTree,to,bend,Reach,0.f,tb))
    return;
  // the hand keeps its turn: the bow upright (canted as it is turned)
  rotateAbout(tr,lHandTree,mul(turn,transpose(rotationOf(tr[lHand]))),position(tr[lHand]));
  bowMiss = length(position(tr[lHand])-to);
  }

Vec3 BowDraw::releaseOffset(const Matrix4x4* tr, size_t numBones, const Vec3& axis) const {
  if(!releaseOn)
    return axis*(-follow);
  const float back = relBack*relFade;
  const float out  = relOut*relFade;
  if(back<=0.f && out<=0.f)
    return Vec3();
  // out: across the arrow, level, away from the head (the hand's side of it)
  Vec3 side = cross(Up,axis);
  if(length(side)<1e-3f)
    return axis*(-back);
  side = normalize(side);
  if(head<numBones && hand<numBones && dot(side,position(tr[hand])-position(tr[head]))<0.f)
    side = side*-1.f;
  return axis*(-back) + side*out;
  }

float BowDraw::followBefore() const {
  if(followT>=FollowT)
    return 0.f;
  const float bump = std::sin(float(M_PI)*followT/FollowT);
  return Follow*bump*bump;
  }

Vec3 BowDraw::clearHead(const Matrix4x4* tr, size_t numBones, const Vec3& base, const Vec3& off) const {
  if(head>=numBones || off==Vec3())
    return off;
  // the hand goes back along the cheek to the ear: off the head's axis as far as it has to
  const Vec3  h0 = position(tr[head]);
  const Vec3  h1 = crown<numBones ? position(tr[crown]) : h0+Vec3(0,20,0);
  const Vec3  hd = h1-h0;
  const float hl = dot(hd,hd);
  auto        on = [&](const Vec3& p) { return h0 + hd*(hl>0.f ? std::clamp(dot(p-h0,hd)/hl,0.f,1.f) : 0.f); };
  const float limit = std::min(length(base-on(base))-HeadR,-HeadIn);
  const Vec3  p  = base+off;
  const Vec3  c  = on(p);
  const Vec3  r  = p-c;
  const float gap = length(r)-HeadR;
  if(gap>=limit || length(r)<1e-3f)
    return off;
  return off + normalize(r)*(limit-gap);
  }

void BowDraw::turnBow(Matrix4x4* tr, const Vec3& up, const Vec3& axis) {
  if(bowTurn==0.f || bowHand==size_t(-1))
    return;
  // about the grip (the bow is slid up in the hand that far), the top limb toward the arrow's way
  const Vec3 n = cross(up,axis);
  if(length(n)<1e-3f)
    return;
  const Mat3 r    = axisAngle(normalize(n),bowTurn*float(M_PI)/180.f);
  const Vec3 at   = position(tr[bowHand]);
  const Vec3 grip = at-slid;
  slid = mul(r,slid);
  tr[bowHand] = compose(mul(r,rotationOf(tr[bowHand])),grip+slid);
  }

void BowDraw::cantHand(Matrix4x4* tr, const Vec3& at, const Vec3& axis, float lean) {
  const Mat3 r = axisAngle(axis,lean);
  if(!moveHand(tr,at+mul(r,position(tr[hand])-at)))
    return;
  rotateAbout(tr,handTree,r,position(tr[hand]));
  }

void BowDraw::measurePose(const Matrix4x4* tr, size_t numBones, const Vec3& slotAt, const Vec3& axis,
                          const Vec3& up, const Vec3& nock, const Vec3& handAnim) {
  // where the arrow passes the bow, along the limbs: over the grip, over the bow's middle
  auto over = [&](const Vec3& p) {
    const Vec3 d = slotAt-p;
    return dot(d - axis*dot(d,axis),up);
    };
  Vec3 rest, pull;
  stringNow(tr,rest,pull);
  overGrip = over(position(tr[bowHand])-slid);
  atMiddle = over(rest);
  square   = std::acos(std::clamp(std::abs(dot(up,axis)),0.f,1.f))*180.f/float(M_PI);
  // upright: the limbs in the plane of the arrow and up
  const Vec3 forward = normalize(Vec3(frameNow.at(2,0),0,frameNow.at(2,2)));
  const Vec3 vert = normalize(active ? AimFrame::up(axis,forward) : Up-axis*axis.y);
  const Vec3 lean = normalize(up - axis*dot(up,axis));
  cant     = std::acos(std::clamp(dot(vert,lean),-1.f,1.f))*180.f/float(M_PI);
  // the nock goes as the hand does (the reload: none till the hand brings the next arrow)
  const Vec3 at = nock + (hand<numBones ? position(tr[hand])-handAnim : Vec3());
  nockGap  = holding && toString<1.f ? -1.f : length(at - (rest+pull*shown));
  }

void BowDraw::reload(Matrix4x4* tr, size_t numBones, const Vec3& nock, const Vec3& axis) {
  // the hand from the aim (the upper body is held in it) round the head on its side to the quiver, where
  // the animation takes the arrow, then round the head again to the string
  if(chest>=numBones || head>=numBones || crown>=numBones)
    return;
  const Vec3 hA = position(tr[hand]);
  const Mat3 rA = rotationOf(tr[hand]);
  const Vec3 eA = position(tr[fore])-position(tr[upper]);
  Vec3 hB = hA, eB = eA;
  Mat3 rB = rA;
  if(grabKnown) {
    const Matrix4x4 g = tr[chest]*grabHand;
    hB = position(g);
    rB = rotationOf(g);
    eB = position(tr[chest]*grabElbow)-position(tr[upper]);
    }
  // round the head's axis on the side of the drawing shoulder
  const Vec3  h0   = position(tr[head]);
  const Vec3  hx   = position(tr[crown])-h0;
  const float hl   = dot(hx,hx);
  if(hl<=0.f)
    return;
  const Vec3  u    = hx*(1.f/std::sqrt(hl));
  const Vec3  c    = h0 + hx*0.5f;
  const Vec3  sh   = position(tr[upper])-c;
  const Vec3  e1   = normalize(sh - u*dot(sh,u));
  const Vec3  e2   = cross(u,e1);
  if(length(e1)<0.5f)
    return;

  Vec3  at;
  Mat3  turn;
  Vec3  elbow;
  float s;
  if(toString<=0.f) {
    s     = toQuiver;
    at    = roundAxis(hA,hB,c,u,e1,e2,RoundOut,s);
    turn  = toMat3(slerp(toQuat(rA),toQuat(rB),s));
    elbow = eA + (eB-eA)*s;
    // a short draw let go: the hand from where it was
    at   += (nock-hA)*(shotAt/std::max(length(nock-hA),1.f))*(1.f-s);
    } else {
    s     = toString;
    at    = roundAxis(hB,nock,c,u,e1,e2,RoundOut,s);
    turn  = toMat3(slerp(toQuat(rB),toQuat(rA),s));
    elbow = eB + (eA-eB)*s;
    }
  // the follow through: back past the face (to the ear), the way to the quiver goes on from there
  if(toString<=0.f) {
    // let go of slowly at first: the hand is off to the quiver as soon as the reload starts (40 ms after the
    // shot), the follow through would hardly show
    const Vec3 off = clearHead(tr,numBones,at,releaseOffset(tr,numBones,axis)*(1.f-toQuiver*toQuiver));
    relShown = length(off);
    seen     = off + axis*followBefore();
    at += off;
    } else {
    seen     = axis*followBefore();
    }
  if(!moveHand(tr,at,&elbow))
    return;
  rotateAbout(tr,handTree,mul(turn,transpose(rotationOf(tr[hand]))),position(tr[hand]));
  miss = length(position(tr[hand])-at);
  }

void BowDraw::implApply(Matrix4x4* tr, size_t numBones) {
  relShown = 0;
  seen     = Vec3();
  travel   = 0;
  miss     = 0;
  shown    = 1;
  bowMove  = 0;
  bowMiss  = 0;
  overGrip = atMiddle = nockGap = square = cant = 0;
  slid     = Vec3();
  // the own clip's start is taken anew each reload: from the hand shown as it begins
  if(!holding)
    ownStarted = false;
  if(!valid || upperTree.back()>=numBones || foreTree.back()>=numBones || handTree.back()>=numBones ||
     slot>=numBones || bowHand>=numBones)
    return;

  // the arrow along -X of the slot (the slot is on the shaft, the nock behind it); drawn fully by the
  // animation
  const Vec3  slotAt = position(tr[slot]);
  const Vec3  axis   = arrowAxis(tr[slot]);
  const Vec3  grip   = position(tr[bowHand]);
  const float ahead  = dot(grip-slotAt,axis);
  // the way the nock goes as the string is let down, the full draw of the animation to the string at rest:
  // the string's middle (the bow's morph, drawn straight back as the bow is held by hand), else straight
  // forward along the arrow
  Vec3 way = axis;
  Vec3 rest, pull;
  fit = offset = angle = -1;
  if(hasString) {
    stringNow(tr,rest,pull);
    span = length(pull);
    } else {
    span = given>0.f ? given : ahead-Brace;
    }
  if(ahead<=0.f || span<=0.f || span>MaxSpan) {
    // the bow is not ahead of the arrow: not the aim pose
    span = 0;
    return;
    }

  // the hand holds the string at the nock, where the animation draws it (on the arrow's line); the bow
  // held so that it is drawn straight back to there, the hand going straight along the arrow
  Vec3 shift = {};
  Vec3 up    = {};
  bool bowHeld = false; // the bow held as an archer holds it: it turns in the hand after the shot
  Vec3 nock  = slotAt;
  if(hasString) {
    const Vec3 high = turnOf(tr[bowHand],stringHigh);
    up = normalize(turnOf(tr[bowHand],bowAxis));
    // the limbs up: as the arrow is up (shot up or down too)
    const Vec3 forward = normalize(Vec3(frameNow.at(2,0),0,frameNow.at(2,2)));
    const Vec3 vert = active ? AimFrame::up(axis,forward) : Up-axis*axis.y;
    if(dot(up,vert)<0.f)
      up = up*-1.f;
    // where the animation draws the string to: there the hand holds it
    nock = slotAt + axis*dot(rest+high-slotAt,axis);
    if(align>0.f && length(up)>0.5f && (active || length(vert)>0.1f) &&
       !lHandTree.empty() && lHandTree.back()<numBones) {
      // upright, or canted for a quick shot: round the arrow, the top limb toward the drawing hand (the side
      // of its shoulder), the fingers on the string with it. So much from upright whatever the animation
      // leans it (the trained ones hold the bow canted ~60 degrees: canted more, it would lie flat)
      float lean = 0;
      if(lUpper<numBones) {
        const Vec3  v    = normalize(vert);
        const Vec3  u    = normalize(up - axis*dot(up,axis));
        const float roll = std::atan2(dot(axis,cross(v,u)),dot(v,u));
        const float side = dot(cross(axis,v),position(tr[upper])-position(tr[lUpper]));
        const float want = cantK*CantAngle*float(M_PI)/180.f*(side>=0.f ? 1.f : -1.f);
        lean = align*(want-roll);
        if(std::abs(lean)>1e-4f)
          cantHand(tr,slotAt,axis,lean); else
          lean = 0;
        }
      alignBow(tr,up,nock,axis,lean);
      bowHeld = true;
      stringNow(tr,rest,pull);
      // the limbs as the bow is held now
      if(lean!=0.f)
        up = mul(axisAngle(axis,lean),up);
      // the bow arm short of it: the hand goes with the string
      shift = rest+pull-nock;
      }
    fit    = length(cross(rest+pull-slotAt,axis));
    offset = length(cross(rest-slotAt,axis));
    angle  = std::acos(std::clamp(dot(normalize(pull)*-1.f,axis),-1.f,1.f))*180.f/float(M_PI);
    // the hand straight along the arrow: not to the side the string is drawn to (the arrow passes beside
    // the bow)
    const Vec3 side = normalize(cross(up,axis));
    const Vec3 back = pull - side*(dot(pull,side)*align);
    span   = length(back);
    way    = back*(-1.f/span);
    }

  // that way as far as the arm reaches: |hand + way*t - shoulder| <= Comfort*arm
  const Vec3  handAnim = position(tr[hand]);
  const Vec3  h0    = handAnim + shift;
  const Vec3  p     = h0 - position(tr[upper]);
  const float arm   = Comfort*(length(position(tr[fore])-position(tr[upper])) + length(h0-position(tr[fore])));
  const float pw    = dot(p,way);
  const float disc  = pw*pw - dot(p,p) + arm*arm;
  const float reach = disc>0.f ? std::max(0.f, -pw + std::sqrt(disc)) : 0.f;
  const float full  = std::min(span,reach);

  if(holding) {
    // the reload: the string rests till the nock is at it, then shows drawn as far as it is (a little)
    shown = (1.f - full/span)*smooth((toString-0.8f)/0.2f);
    if(own && ownHeld) {
      ownReload(tr,numBones,h0+way*full,axis);
      } else {
      ownStarted = false;
      reload(tr,numBones,h0+way*full,axis);
      }
    if(bowHeld)
      turnBow(tr,up,axis);
    if(hasString)
      measurePose(tr,numBones,slotAt,axis,up,nock,handAnim);
    return;
    }
  if(bowHeld)
    turnBow(tr,up,axis);
  if(hasString)
    measurePose(tr,numBones,slotAt,axis,up,nock,handAnim);
  if(weight<=0.f && relBack*relFade<=0.f)
    return;

  // the whole draw over what the arm reaches: the hand moves all the way, the string with it
  travel = full*(1.f-draw)*weight;
  shown  = 1.f - travel/span;
  // the follow through: back past the face
  const Vec3 off  = clearHead(tr,numBones,position(tr[hand])+shift+way*travel,releaseOffset(tr,numBones,axis));
  relShown = length(off);
  seen     = off + axis*followBefore();
  const Vec3 move = shift + way*travel + off;
  if(length(move)<0.01f)
    return;
  const Vec3 target = position(tr[hand]) + move;
  if(!moveHand(tr,target))
    return;
  miss = length(position(tr[hand])-target);
  if(hasString)
    measurePose(tr,numBones,slotAt,axis,up,nock,handAnim);
  }

void BowDraw::probe(Probe& p) const {
  p.valid   = valid;
  p.weight  = weight;
  p.draw    = draw;
  p.span    = span;
  p.travel  = travel;
  p.miss    = miss;
  p.follow  = follow;
  p.back    = relShown;
  p.kick    = bowKick;
  p.turn    = bowTurn;
  p.fit     = fit;
  p.offset  = offset;
  p.angle   = angle;
  p.overGrip = overGrip;
  p.atMiddle = atMiddle;
  p.nockGap  = nockGap;
  p.square   = square;
  p.cant     = cant;
  p.bowMove  = bowMove;
  p.bowMiss  = bowMiss;
  p.aligned  = align;
  p.holding = holding;
  p.nock    = toString>0.f ? 1.f+toString : (holding && own ? smooth(reloadP/OwnTake) : toQuiver);
  p.handAt  = handAt;
  p.head    = gapHand;
  p.fore    = gapFore;
  p.arrow   = gapArrow;
  p.own        = rq.own;
  p.reload     = rq.reload;
  p.quiver     = rq.quiver;
  p.qMouth     = rq.qMouth;
  p.qAxis      = rq.qAxis;
  p.headAt     = rq.headAt;
  p.crownAt    = rq.crownAt;
  p.shoulderAt = rq.shoulderAt;
  p.gripAt     = rq.gripAt;
  p.nockAt     = rq.nockAt;
  p.slotAt     = rq.slotAt;
  p.arrowNock  = rq.arrowNock;
  p.arrowTip   = rq.arrowTip;
  p.foreAt     = rq.foreAt;
  p.torsoHand  = rq.torsoHand;
  p.torsoFore  = rq.torsoFore;
  p.torsoArrow = rq.torsoArrow;
  p.tubeHand   = rq.tubeHand;
  p.tubeFore   = rq.tubeFore;
  p.tipIn      = rq.tipIn;
  p.offAxis    = rq.offAxis;
  p.led        = rq.led;
  }

void BowDraw::ownReload(Matrix4x4* tr, size_t numBones, const Vec3& nock, const Vec3& axis) {
  // the own clip (QuiverReloadAnim): the drawing arm and the head as the clip has them relative to the neck, over
  // the held aim. The hand starts where the aim and the follow through had it, is led to the quiver as it hangs
  // round the grab and the draw out of it, and to the string as the aim has it at the end
  if(chest>=numBones || head>=numBones || neck>=numBones || clav>=numBones || pelvis>=numBones)
    return;
  const float     p   = std::clamp(reloadP,0.f,1.f);
  const Vec3      hA  = position(tr[hand]);
  const Mat3      rA  = rotationOf(tr[hand]);
  const Vec3      eA  = position(tr[fore])-position(tr[upper]);
  const Matrix4x4 nk  = tr[neck];
  Matrix4x4 nkInv = nk;
  nkInv.inverse();
  Matrix4x4 heldInv = clavHeld;
  heldInv.inverse();
  // the aim turns the arms about the chest (Aim): the drawing arm leaves that turn on its way to the quiver and
  // takes it again on its way to the bow
  Matrix4x4 none;
  none.identity();
  const Matrix4x4 turnFull = nkInv*tr[clav]*heldInv;
  const float     wArm     = p<OwnOut ? 1.f-smooth((p-OwnLeave)/(OwnGrab-OwnLeave)) : smooth((p-OwnOut)/(OwnShelf-OwnOut));
  const Matrix4x4 base     = nk*blend(none,turnFull,wArm);
  for(auto i:armTree)
    tr[i] = base*ownRel[i];
  // the head looks as the clip has it from its first frame
  if(ownKnown) {
    Matrix4x4 h0 = ownHead0;
    h0.inverse();
    const Matrix4x4 look = h0*ownRel[head];
    const Matrix4x4 hm   = tr[head];
    Matrix4x4 hmInv = hm;
    hmInv.inverse();
    const Matrix4x4 m = hm*look*hmInv;
    for(auto i:headTree)
      tr[i] = m*tr[i];
    }

  Vec3 at   = position(tr[hand]);
  Mat3 turn = rotationOf(tr[hand]);
  Vec3 bend = position(tr[fore])-position(tr[upper]);
  // from where the hand was as the reload began (the aim, the follow through, a short draw), the clip's way from
  // there on, by the grab all of it
  seen = axis*followBefore();
  if(ownKnown && toQuiver<1.f) {
    const Matrix4x4 k0  = nk*turnFull*ownHand0;
    const Vec3      off = clearHead(tr,numBones,hA,releaseOffset(tr,numBones,axis));
    if(!ownStarted) {
      ownStart   = handAt;
      ownStarted = true;
      }
    Vec3            start = ownStart;
    frameNow.project(start);
    const float     k   = 1.f-toQuiver*toQuiver;
    at  += (start-position(k0))*k;
    turn = mul(toMat3(slerp(Quat(),toQuat(mul(rA,transpose(rotationOf(k0)))),k)),turn);
    relShown = length(off)*k;
    seen    += off*k;
    }
  if(!ownKnown)
    ownStarted = false;
  // the quiver as it hangs (the give, the gait's turn, the armour's push) against the one the clip reaches
  qFollow = p<OwnOut ? smooth((p-OwnLeave)/(OwnGrab-OwnLeave)) : 1.f-smooth((p-OwnOut)/(OwnShelf-OwnOut));
  // the arrow's tip out of the tube (past K3): the quiver goes back alone, the hand keeps the lead it had till the
  // reload is over (the arrow turned toward the bow may dip under the mouth's plane again, beside the tube)
  if(p<OwnTake)
    leadFrozen = false;
  else if(p>=OwnOut-0.1f && rq.own && rq.tipIn<=0.f && !leadFrozen && quiverKnown) {
    leadFrozen = true;
    lead       = quiverNow;
    }
  if(quiverKnown && qFollow>0.f) {
    Matrix4x4 clip = nk*blend(none,turnFull,wArm)*quiverOfNeck;
    clip.inverse();
    const Matrix4x4 lead = tr[pelvis]*(leadFrozen ? this->lead : quiverNow)*clip;
    const Matrix4x4 led  = blend(compose(turn,at),lead*compose(turn,at),qFollow);
    at   = position(led);
    turn = rotationOf(led);
    bend = bend + (mul(rotationOf(lead),bend)-bend)*qFollow;
    }
  // the string as the aim has it (the hand's turn of the aim)
  if(toString>0.f) {
    at   = at + (nock-at)*toString;
    turn = toMat3(slerp(toQuat(turn),toQuat(rA),toString));
    bend = bend + (eA-bend)*toString;
    }
  if(!moveHand(tr,at,&bend))
    return;
  rotateAbout(tr,handTree,mul(turn,transpose(rotationOf(tr[hand]))),position(tr[hand]));
  miss = length(position(tr[hand])-at);
  }

void BowDraw::measureReload(const Matrix4x4* tr, size_t numBones) {
  rq = Probe();
  drawn = -1;
  rq.own    = holding && own && ownHeld;
  rq.reload = holding ? reloadP : -1.f;
  rq.led    = rq.own ? qFollow : 0.f;
  if(!valid || chest>=numBones || head>=numBones || hand>=numBones || fore>=numBones || slot>=numBones)
    return;
  Matrix4x4 inv = tr[chest];
  inv.inverse();
  auto in = [&](const Vec3& w) { Vec3 v = w; inv.project(v); return v; };
  rq.headAt     = in(position(tr[head]));
  rq.crownAt    = crown<numBones ? in(position(tr[crown])) : rq.headAt;
  rq.shoulderAt = in(position(tr[upper]));
  rq.gripAt     = bowHand<numBones ? in(position(tr[bowHand])-slid) : Vec3();
  rq.slotAt     = in(position(tr[slot]));
  rq.foreAt     = in(position(tr[fore]));
  const Vec3 ax = arrowAxis(tr[slot]);
  const Vec3 sl = position(tr[slot]);
  rq.arrowNock  = in(sl-ax*ArrowNock);
  rq.arrowTip   = in(sl+ax*ArrowTip);
  if(hasString && bowHand<numBones) {
    Vec3 rest, pull;
    stringNow(tr,rest,pull);
    rq.nockAt = in(rest);
    }
  // the trunk: in the chest's frame, round the line from the pelvis up the spine
  const Vec3 pv = pelvis<numBones ? in(position(tr[pelvis])) : Vec3(-25,0,0);
  auto trunk = [&](const Vec3& q) {
    if(q.x>TrunkTop || q.x<pv.x)
      return 99.f;
    const float t  = (q.x-pv.x)/std::max(-pv.x,1.f);
    const float cy = pv.y*(1.f-std::clamp(t,0.f,1.f)), cz = pv.z*(1.f-std::clamp(t,0.f,1.f));
    const float y  = q.y-cy, z = q.z-cz;
    const float r  = std::sqrt(y*y+z*z);
    const float k  = std::sqrt((y/TrunkWide)*(y/TrunkWide)+(z/TrunkDeep)*(z/TrunkDeep));
    return k>1e-4f ? r*(1.f-1.f/k) : -std::min(TrunkWide,TrunkDeep);
    };
  auto along = [&](const Vec3& a, const Vec3& b, auto f) {
    float best = 99.f;
    for(int i=0; i<=8; ++i)
      best = std::min(best,f(a+(b-a)*(float(i)/8.f)));
    return best;
    };
  const Vec3 hd = in(position(tr[hand]));
  const Vec3 fm = (rq.foreAt+hd)*0.5f;
  rq.torsoHand  = trunk(hd);
  rq.torsoFore  = along(fm,hd,trunk);
  rq.torsoArrow = inHand ? along(rq.arrowNock,rq.arrowTip,trunk) : 99.f;
  if(!quiverKnown || pelvis>=numBones)
    return;
  const Matrix4x4 q = tr[pelvis]*quiverNow;
  const Vec3 mouth  = position(q);
  const Vec3 down   = normalize(Vec3(-q.at(1,0),-q.at(1,1),-q.at(1,2)));
  rq.quiver = true;
  rq.qMouth = in(mouth);
  rq.qAxis  = in(mouth+down)-rq.qMouth;
  // the tube: from the mouth to the bottom, narrowing
  auto tube = [&](const Vec3& w) {
    const float t = dot(w-mouth,down);
    if(t<0.f || t>Quiver::Length)
      return 99.f;
    const float r = Quiver::MouthRadius + (Quiver::BottomRadius-Quiver::MouthRadius)*t/Quiver::Length;
    return length(w-mouth-down*t)-r;
    };
  const Vec3 hw = position(tr[hand]);
  const Vec3 fw = (position(tr[fore])+hw)*0.5f;
  rq.tubeHand = tube(hw);
  rq.tubeFore = along(fw,hw,tube);
  if(inHand) {
    const Vec3 tip = sl+ax*ArrowTip, nk = sl-ax*ArrowNock;
    rq.tipIn = dot(tip-mouth,down);
    if(rq.tipIn>0.f) {
      // the arrow's points under the mouth: how far off the axis
      float off = 0;
      for(int i=0; i<=16; ++i) {
        const Vec3  w = nk+(tip-nk)*(float(i)/16.f);
        const float t = dot(w-mouth,down);
        if(t>=0.f)
          off = std::max(off,length(w-mouth-down*t));
        }
      rq.offAxis = off;
      }
    if(rq.own && rq.tipIn>0.f && reloadP>=OwnTake-0.02f && reloadP<OwnShelf)
      drawn = std::max(0.f,TipDepth-rq.tipIn);
    }
  }
