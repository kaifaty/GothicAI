#include "npc_melee_shadow.h"

#include <Tempest/Matrix4x4>

#include "graphics/mesh/skeleton.h"
#include "graphics/effect.h"
#include "game/damagecalculator.h"
#include "game/bodyobservations.h"
#include "game/bodyarena.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "utils/dbgpainter.h"
#include "camera.h"
#include "testbench.h"
#include "gothic.h"
#include "resources.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>

using namespace Tempest;

namespace {

constexpr float meleeMagnetTorsoGapCm = 18.f;

std::string meleeShadowPoint(const Vec3& p) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << '[' << p.x << ',' << p.y << ',' << p.z << ']';
  return out.str();
  }

std::string meleeShadowBone(const MeleeBladeContact& hit) {
  const auto* skeleton = hit.npc!=nullptr ? hit.npc->visualSkeleton() : nullptr;
  return skeleton!=nullptr && hit.bone<skeleton->nodes.size() ? skeleton->nodes[hit.bone].name : "";
  }

}

namespace NpcMeleeShadow {

std::string meleeShadowWho(const Npc* npc) {
  if(npc==nullptr)
    return "null";
  const auto* symbol = npc->world().script().findSymbol(npc->instanceSymbol());
  return "{\"id\":" + std::to_string(npc->world().npcId(npc)) +
         ",\"instance\":" + std::to_string(npc->instanceSymbol()) + ",\"name\":" +
         TestBench::bodyJson(symbol!=nullptr ? std::string_view(symbol->name()) : std::string_view("npc")) +
         ",\"position_cm\":" + meleeShadowPoint(npc->position()) + "}";
  }

void writeMeleeShadow(const std::string& row) {
  // NPC animation updates run on workers. Keep complete rows and rotation under one lock.
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  static std::ofstream out;
  static uintmax_t bytes = 0;
  constexpr uintmax_t limit = 8*1024*1024;
  const char* path = "melee-blade-shadow.jsonl";
  const auto entry = "{\"arena_seed\":" + (BodyArena::enabled() ? std::to_string(BodyArena::seed()) : "null") + ',' + row.substr(1);
  if(!out.is_open()) {
    std::error_code error;
    bytes = std::filesystem::file_size(path,error);
    if(error)
      bytes = 0;
    out.open(path,std::ios::app);
    }
  if(bytes+entry.size()+1>limit) {
    out.close();
    std::error_code error;
    std::filesystem::remove("melee-blade-shadow.previous.jsonl",error);
    std::filesystem::rename(path,"melee-blade-shadow.previous.jsonl",error);
    out.open(path,std::ios::trunc);
    bytes = 0;
    }
  if(out) {
    out << entry << '\n';
    out.flush();
    bytes += entry.size()+1;
    }
  }

}

Npc::MeleeBenchMode Npc::meleeBenchMode() const {
  if(meleeBench!=MeleeBenchMode::Off)
    return meleeBench;
  static const MeleeBenchMode runnerMode = [] {
    const auto* value = std::getenv("OPENGOTHIC_MELEE_BENCH");
    if(value!=nullptr && std::string_view(value)=="blade-motors")
      return MeleeBenchMode::Full;
    if(value!=nullptr && std::string_view(value)=="blade")
      return MeleeBenchMode::Blade;
    return MeleeBenchMode::Off;
    }();
  if(runnerMode==MeleeBenchMode::Off)
    return MeleeBenchMode::Off;
  return BodyObservations::watched(*this) ? runnerMode : MeleeBenchMode::Off;
  }

void Npc::configureMeleeBench(MeleeBenchMode mode, bool dynamicPelvis) {
  const auto* value = std::getenv("OPENGOTHIC_MELEE_BENCH");
  if(value==nullptr || std::string_view(value)!="fixture")
    return;
  meleeBench = mode;
  meleeBenchDynamicPelvis = dynamicPelvis;
  living.reset();
  livingRetryTick = 0;
  livingNeedsRebuild = false;
  prepareLivingBody();
  }

void Npc::updateMeleeBenchBlade() {
  const auto mode = meleeBenchMode();
  if(mode==MeleeBenchMode::Off || mode==MeleeBenchMode::Base || mode==MeleeBenchMode::Chain)
    return;
  // Full fixture takes isolate motors. Runner full takes also include their blade.
  if(mode==MeleeBenchMode::Full && meleeBench!=MeleeBenchMode::Off)
    return;
  const auto* weapon = invent.activeWeapon();
  const auto* skeleton = visual.visualSkeleton();
  if(weapon==nullptr || skeleton==nullptr || weapon->swordLength()<=0)
    return;
  const auto hand = skeleton->findNode("ZS_RIGHTHAND");
  if(hand<visual.pose().boneCount()) {
    const auto* mesh=Resources::loadMesh(weapon->handle().visual);
    if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
      throw std::runtime_error("Melee benchmark requires a weapon mesh extending along local -X");
    owner.physic()->updateMeleeBlade(this,visual.pose().bone(hand),-mesh->bbox()[0].x);
    }
  }

std::optional<std::pair<uint32_t,uint32_t>> Npc::meleeFinishStroke(const std::vector<float>& height) {
  // the longest drop of the tip
  uint32_t top = 0, from = 0, to = 0;
  for(uint32_t frame=1;frame<height.size();++frame) {
    if(height[frame]>height[top])
      top = frame;
    if(height[top]-height[frame]>height[from]-height[to])
      from = top, to = frame;
    }
  // the stroke ends where the tip comes to rest at its lowest, not on the later settling,
  // and starts at the top it falls from, not on a higher point before the swing round the side
  while(to>from+1 && height[to-1]-height[to]<=2.f)
    --to;
  from = to;
  while(from>0 && height[from-1]>=height[from]-1.f)
    --from;
  if(to<=from+1)
    return std::nullopt;
  return std::make_pair(from,to);
  }

void Npc::cacheMeleeShadowWindows(const Animation::Sequence* seq, float length, bool normalY) const {
  if(!meleeShadow)
    meleeShadow = std::make_unique<MeleeShadow>();
  auto& shadow = *meleeShadow;
  if(std::any_of(shadow.windows.begin(),shadow.windows.end(),[&](const MeleeShadow::Window& w) {
       return w.sequence==seq && w.length==length && w.normalY==normalY;
       }))
    return;
  const auto mode = weaponState();
  const auto* skeleton = visual.visualSkeleton();
  const auto& data = *seq->data;
  // A scratch pose samples the selected MAN without changing the actor's
  // animation clocks, events or RNG.
  Pose sampled;
  sampled.setSkeleton(skeleton);
  AnimationSolver solver;
  solver.setSkeleton(skeleton);
  sampled.startAnim(solver,seq,0,BS_HIT,Pose::Force,0);
  const auto& pose = sampled;
  // Sample the actual MAN hand path in the actor's frame. Locomotion cannot
  // open a stationary blade's window. Adjacent optimal tags partition combos.
  const auto root = skeleton->findRootNode();
  const auto right = skeleton->findNode(mode==WeaponState::Fist ? "BIP01 R HAND" : "ZS_RIGHTHAND");
  const auto left = skeleton->findNode("BIP01 L HAND");
  if(right>=pose.boneCount() || root>=pose.boneCount() || data.numFrames<2 || data.fpsRate<=0)
    throw std::runtime_error("Melee shadow requires a sampled humanoid attack and hand bone");
  std::vector<uint32_t> optimal;
  for(const auto& event:data.events)
    if(event.type==asset::MdsEventType::OPTIMAL_FRAME)
      for(const auto frame:event.frames) {
        // The same frame as Sequence::processEvents, including truncated MAN ranges.
        const auto clamped = data.optimalFrame(frame);
        optimal.push_back(seq->reverse ? data.numFrames-1-clamped : clamped);
        }
  std::sort(optimal.begin(),optimal.end());
  optimal.erase(std::unique(optimal.begin(),optimal.end()),optimal.end());
  std::vector<float> rightSpeed(data.numFrames,0), leftSpeed(data.numFrames,0), rightReach(data.numFrames,0), leftReach(data.numFrames,0);
  const auto sampleSpeeds = [&](size_t hand,std::vector<float>& speeds,std::vector<float>& reach) {
    Vec3 previous;
    for(uint32_t frame=0;frame<data.numFrames;++frame) {
      const float progress = float(frame)/float(data.numFrames-1);
      Matrix4x4 matrix;
      if(!pose.animRelative(seq->name,seq->reverse ? 1.f-progress : progress,root,hand,matrix))
        throw std::runtime_error("Cannot sample melee shadow attack hand");
      Vec3 point(mode==WeaponState::Fist ? 0.f : -length,0,0);
      matrix.project(point);
      reach[frame] = Vec2(point.x,point.z).length();
      if(frame>0)
        speeds[frame] = (point-previous).length()*data.fpsRate;
      previous = point;
      }
    speeds[0] = speeds[1];
    };
  sampleSpeeds(right,rightSpeed,rightReach);
  if(mode==WeaponState::Fist && left<pose.boneCount())
    sampleSpeeds(left,leftSpeed,leftReach);
  // The original's finishing move (T_1HSFINISH, T_2HSFINISH) has no optimal frame: it kills by
  // script. As a chop at one lying, its blow is the stroke down: the longest drop of the blade's
  // tip, from the top of the swing to the ground. The blade is fastest earlier, swung round the
  // side over the head, and a window placed there closed with a short axe still above the body.
  std::optional<std::pair<uint32_t,uint32_t>> stroke;
  if(optimal.empty() && mode!=WeaponState::Fist && !seq->reverse) {
    std::vector<float> height(data.numFrames,0);
    for(uint32_t frame=0;frame<data.numFrames;++frame) {
      Matrix4x4 matrix;
      if(!pose.animRelative(seq->name,float(frame)/float(data.numFrames-1),size_t(-1),right,matrix))
        throw std::runtime_error("Cannot sample melee shadow finishing stroke");
      Vec3 tip(-length,0,0);
      matrix.project(tip);
      height[frame] = tip.y;
      }
    stroke = meleeFinishStroke(height);
    }
  if(optimal.empty())
    optimal.push_back(stroke ? uint32_t(std::max_element(rightSpeed.begin()+stroke->first,rightSpeed.begin()+stroke->second+1)-rightSpeed.begin()) :
                               uint32_t(std::max_element(rightSpeed.begin(),rightSpeed.end())-rightSpeed.begin()));
  for(uint32_t index=0;index<optimal.size();++index) {
    const auto opt = optimal[index];
    const uint32_t lo = stroke ? stroke->first : index==0 ? 0 : (optimal[index-1]+opt)/2+1;
    const uint32_t hi = stroke ? stroke->second : index+1==optimal.size() ? data.numFrames-1 : (opt+optimal[index+1])/2;
    const float rpeak = *std::max_element(rightSpeed.begin()+lo,rightSpeed.begin()+hi+1);
    const float lpeak = *std::max_element(leftSpeed.begin()+lo,leftSpeed.begin()+hi+1);
    const bool useLeft = mode==WeaponState::Fist && lpeak>rpeak;
    const auto& speeds = useLeft ? leftSpeed : rightSpeed;
    const float peak = std::max(rpeak,lpeak);
    const float threshold = std::max(100.f,peak*.25f);
    uint32_t first = opt, last = opt;
    while(first>lo && speeds[first]>=threshold)
      --first;
    while(last<hi && speeds[last+1]>=threshold)
      ++last;
    if(stroke) {
      first = stroke->first;
      last = stroke->second;
      }
    // A low-speed optimal frame still has its own adjacent sample interval.
    if(first==last) {
      first = first>lo ? first-1 : first;
      last = last<hi ? last+1 : last;
      }
    const uint32_t end = seq->reverse ? std::max(last+1,opt+2) : last+1;
    shadow.windows.push_back({seq,length,index,first,opt,last,float(first)*1000.f/data.fpsRate,
                              float(opt)*1000.f/data.fpsRate,float(end)*1000.f/data.fpsRate,
                              peak,threshold,useLeft,normalY,float(opt+(seq->reverse ? 1u : 0u))*1000.f/data.fpsRate,
                              *std::max_element((useLeft ? leftReach : rightReach).begin()+first,(useLeft ? leftReach : rightReach).begin()+last+1)});
    }
  }

Npc::MeleeReach Npc::sampleMeleeReach(const Npc& target, Pose& sampled, const Pose::CombatLayer& attack, float yawDelta) const {
  MeleeReach result;
  const auto mode = weaponState();
  const auto* item = mode==WeaponState::Fist ? nullptr : invent.activeWeapon();
  float length = 12.f;
  bool normalY = true;
  if(item!=nullptr) {
    const auto* mesh = Resources::loadMesh(item->handle().visual);
    if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
      throw std::runtime_error("Physical melee reach requires a weapon mesh extending along local -X");
    length = -mesh->bbox()[0].x;
    const auto extent = mesh->bbox()[1]-mesh->bbox()[0];
    normalY = extent.y<=extent.z;
    }
  cacheMeleeShadowWindows(attack.sequence,length,normalY);
  const MeleeShadow::Window* selected = nullptr;
  const auto combo = uint32_t(sampled.comboLength());
  for(const auto& window:meleeShadow->windows)
    if(window.sequence==attack.sequence && window.length==length && window.normalY==normalY &&
       (selected==nullptr || (window.index<=combo && window.index>selected->index)))
      selected = &window;
  if(selected==nullptr)
    throw std::runtime_error("Physical melee reach requires an active attack window");
  const auto window = *selected;
  auto direction = target.position()-position();
  direction.y = 0;
  const float distance = direction.length();
  if(distance<=0) {
    result.advanceCm = 0;
    result.uncoveredCm = 0;
    return result;
    }
  direction /= distance;
  // Rebase both read-only copies to this tick's transforms. Animation may still
  // carry the preceding frame's position/yaw when native movement turns to hit.
  sampled.setObjectMatrix(mkPositionMatrix(),false);
  sampled.setBlendLog(false);
  Pose targetPose = target.visual.pose();
  targetPose.setBlendLog(false);
  const auto targetMatrix = target.mkPositionMatrix();
  targetPose.setObjectMatrix(targetMatrix,false);
  targetPose.update(owner.tickCount(),true);
  std::vector<HitZones::Posed> zones;
  const float scale = Vec3(targetMatrix.at(0,0),targetMatrix.at(0,1),targetMatrix.at(0,2)).length();
  target.visual.hitZoneShape().posed(targetPose.transform(),targetPose.boneCount(),scale,zones);
  std::vector<MeleeBodyCapsule> body;
  for(const auto& zone:zones)
    body.push_back({zone.a,zone.b,zone.r,zone.bone,uint8_t(zone.zone)});
  const double activeBegin = double(attack.start)+double(window.beginMs)/attack.speed;
  const double activeEnd = double(attack.start)+double(window.endMs)/attack.speed;
  if(activeEnd<=double(owner.tickCount()))
    return result;
  // Half-MAN-frame samples preserve the native blend, height and lateral lane.
  // No events, actor clocks or RNG are advanced by Pose::update on these copies.
  const auto interval = std::max<uint64_t>(1,uint64_t(500.f/(attack.sequence->data->fpsRate*attack.speed)));
  const auto last = uint64_t(std::ceil(activeEnd))-1;
  const auto hand = sampled.findNode(mode==WeaponState::Fist ? (window.left ? "BIP01 L HAND" : "BIP01 R HAND") : "ZS_RIGHTHAND");
  const auto forearm = sampled.findNode(window.left ? "BIP01 L FOREARM" : "BIP01 R FOREARM");
  const auto finger = sampled.findNode(window.left ? "BIP01 L FINGER0" : "BIP01 R FINGER0");
  std::vector<MeleeBladeSegment> blades;
  std::vector<std::pair<float,Vec3>> rootSteps;
  auto previous = owner.tickCount();
  for(uint64_t tick=owner.tickCount();tick<=last;) {
    const float elapsed = float(tick-owner.tickCount());
    if(tick>previous) {
      Vec3 displacement;
      mvAlgo.applyRotation(displacement,sampled.animMoveSpeed(tick,tick-previous));
      displacement.y = 0; // the ordinary human CharacterVirtual root policy
      rootSteps.emplace_back(elapsed,displacement);
      }
    previous = tick;
    sampled.update(tick,true);
    if(double(tick)>=activeBegin && hand<sampled.boneCount()) {
      // Each candidate is a complete, frozen plan ending at this contact time.
      // Root deltas are sampled once, then integrated under that plan's turn.
      const float yawLimit = std::min(30.f,.18f*elapsed);
      const float yaw = std::clamp(yawDelta,-yawLimit,yawLimit);
      const float turn = yaw*float(M_PI)/180.f;
      const float turnSin = std::sin(turn), turnCos = std::cos(turn);
      Vec3 rootMotion;
      for(const auto& [time,displacement]:rootSteps) {
        const float rootTurn = elapsed>0.f ? turn*time/elapsed : 0.f;
        rootMotion += Vec3(std::cos(rootTurn)*displacement.x-std::sin(rootTurn)*displacement.z,0,
                           std::sin(rootTurn)*displacement.x+std::cos(rootTurn)*displacement.z);
        }
      const auto rotate = [&](Vec3 point) {
        const auto offset = point-position();
        return position()+rootMotion+Vec3(turnCos*offset.x-turnSin*offset.z,offset.y,
                                         turnSin*offset.x+turnCos*offset.z);
        };
      const auto at = [&](size_t bone,Vec3 point) { sampled.bone(bone).project(point); return rotate(point); };
      const auto grip = at(hand,{});
      blades.clear();
      if(mode==WeaponState::Fist) {
        const auto knuckle = finger<sampled.boneCount() ? at(finger,{}) : grip;
        blades.push_back({grip,knuckle,{0,1,0},6.f});
        if(forearm<sampled.boneCount()) blades.push_back({at(forearm,{}),grip,{0,1,0},4.f});
        }
      else {
        const auto blade = at(hand,{-length,0,0})-grip;
        // Plan contact on the working outer blade, not beside the grip.
        blades.push_back({grip+blade*.5f,grip+blade*.85f,{0,1,0},2.f});
        }
      std::vector<HitZones::Posed> torso;
      const auto matrix = mkPositionMatrix();
      const float ownScale = Vec3(matrix.at(0,0),matrix.at(0,1),matrix.at(0,2)).length();
      visual.hitZoneShape().posed(sampled.transform(),sampled.boneCount(),ownScale,torso);
      for(auto& capsule:torso) {
        capsule.a = rotate(capsule.a);
        capsule.b = rotate(capsule.b);
        }
      // Cast both ways: a short backward correction can put an overreaching
      // blade back through a close target. Preserve the raw distance for cap statistics.
      const float limit = distance+length+40.f;
      const float forward = owner.physic()->meleeBladeAdvance(blades,body,direction,limit);
      const float backward = owner.physic()->meleeBladeAdvance(blades,body,-direction,limit);
      const auto consider = [&](float advance) {
        if(!std::isfinite(advance))
          return;
        const float gap = HitZones::horizontalBodyGap(torso,zones,direction*advance);
        if(gap<meleeMagnetTorsoGapCm || Vec3::dotProduct(target.position()-position()-rootMotion-direction*advance,direction)<0.f)
          return;
        const float available = std::min(advance<0.f ? 40.f : 120.f,.6f*elapsed);
        const float uncovered = std::max(0.f,std::abs(advance)-available);
        const float delivered = std::min(std::abs(advance),available);
        const float priorAvailable = std::min(result.advanceCm<0.f ? 40.f : 120.f,.6f*result.contactMs);
        const float priorDelivered = std::min(std::abs(result.advanceCm),priorAvailable);
        const bool feasible = uncovered==0.f, priorFeasible = result.uncoveredCm==0.f;
        const bool hasFeed = elapsed>0.f && delivered>0.f;
        const bool priorHasFeed = result.contactMs>0.f && priorDelivered>0.f;
        bool better = false;
        if(feasible!=priorFeasible)
          better = feasible;
        else if(!feasible && hasFeed!=priorHasFeed)
          better = hasFeed;
        else if(uncovered!=result.uncoveredCm)
          better = uncovered<result.uncoveredCm;
        else
          better = feasible ? std::abs(advance)<std::abs(result.advanceCm) : delivered>priorDelivered;
        // An unreachable t=0 cast must not suppress a later partial feed.
        // Genuine zero-time overlap remains a feasible, unassisted contact.
        if(better) {
          result.advanceCm = advance;
          result.contactMs = elapsed;
          result.yawDeg = yaw;
          result.uncoveredCm = uncovered;
          result.torsoGapCm = gap;
          }
        };
      consider(forward);
      consider(-backward);
      if((forward==0.f || backward==0.f) && HitZones::horizontalBodyGap(torso,zones)<meleeMagnetTorsoGapCm) {
        // Starting inside a target makes an ordinary cast return zero in both
        // directions. Cast back from outside to retain a bounded retreat plan.
        auto shifted = blades;
        for(auto& segment:shifted) { segment.from-=direction*limit; segment.to-=direction*limit; }
        consider(owner.physic()->meleeBladeAdvance(shifted,body,direction,2.f*limit)-limit);
        }
      }
    if(tick==last) break;
    tick = std::min(last,tick+interval);
    }
  return result;
  }

void Npc::syncMeleeShadowSwing() {
  const auto mode = weaponState();
  const bool enabled = Gothic::physicsFlags().meleeBladeShadow || Gothic::physicsFlags().meleePhysicsHits;
  if(isMeleeRiposting() || meleeShadowSuspended || !isHuman() || isDown() || meleeKnockdown ||
     (mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H)) {
    finishMeleeShadowSwing(true,meleeShadowSuspended);
    return;
    }
  if(!enabled && !(meleeShadow && meleeShadow->window))
    return;
  const auto& pose = visual.pose();
  std::optional<Pose::CombatLayer> attack;
  for(const auto& layer:pose.combatLayers(owner.tickCount()))
    if(layer.sequence->isAttackAnim() || (meleeDownStrike && isFinishSequence(*layer.sequence)))
      attack = layer;
  if(!attack) {
    meleeDownStrike = false;
    finishMeleeShadowSwing();
    return;
    }
  if(meleeDownStrike && meleeShadow && meleeShadow->window && meleeShadow->window->sequence==attack->sequence &&
     meleeShadow->origin==attack->origin) {
    // The finishing move stays over the body for seconds: the pull-out and the step back.
    // A moment after the blow the rest plays faster; cut short, its step forward stayed
    // (70 cm a chop) and the hero ended up standing on the body.
    constexpr double holdMs = 250;
    constexpr float  restSpeed = 2.5f;
    if(double(owner.tickCount())>=double(attack->start)+double(meleeShadow->window->endMs)/attack->speed+holdMs) {
      meleeDownStrike = false;
      finishMeleeShadowSwing();
      visual.setLayerSpeed(attack->sequence,restSpeed,owner.tickCount());
      return;
      }
    }
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr)
    throw std::runtime_error("Melee shadow requires a humanoid attack skeleton");
  const auto* item = mode==WeaponState::Fist ? nullptr : invent.activeWeapon();
  float length = 12.f;
  bool normalY = true;
  if(item!=nullptr) {
    const auto* mesh = Resources::loadMesh(item->handle().visual);
    if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
      throw std::runtime_error("Melee shadow requires a weapon mesh extending along local -X");
    length = -mesh->bbox()[0].x;
    const auto extent = mesh->bbox()[1]-mesh->bbox()[0];
    normalY = extent.y<=extent.z;
    }
  if(mode!=WeaponState::Fist && item==nullptr)
    return;
  if(!meleeShadow)
    meleeShadow = std::make_unique<MeleeShadow>();
  auto& shadow = *meleeShadow;
  const auto* seq = attack->sequence;
  const auto combo = uint32_t(pose.comboLength());
  const auto cached = [&](const MeleeShadow::Window& w) {
    return w.sequence==seq && w.length==length && w.normalY==normalY;
    };
  cacheMeleeShadowWindows(seq,length,normalY);
  const MeleeShadow::Window* window = nullptr;
  for(const auto& w:shadow.windows)
    if(cached(w) && (window==nullptr || (w.index<=combo && w.index>window->index)))
      window = &w;
  if(window==nullptr)
    throw std::runtime_error("Melee shadow requires an optimal-frame attack window");
  if(shadow.window && shadow.window->sequence==seq && shadow.window->index==window->index &&
     shadow.origin==attack->origin && shadow.window->length==length && shadow.window->normalY==normalY) {
    // the same swing: a hit stop moves its clock, the windows follow it
    shadow.start = attack->start;
    shadow.speed = attack->speed;
    return;
    }
  const auto selected = *window;
  finishMeleeShadowSwing(false);
  if(meleeShadowSuspended)
    return;
  if(!enabled) {
    owner.physic()->removeShadowBlade(this);
    return;
    }
  shadow.window = selected;
  ++shadow.id;
  shadow.start = attack->start;
  shadow.origin = attack->origin;
  shadow.tick = owner.tickCount();
  shadow.speed = attack->speed;
  shadow.attacker = meleeShadowWho(this);
  shadow.animation = seq->name;
  const auto* weaponSymbol = item!=nullptr ? owner.script().findSymbol(item->clsId()) : nullptr;
  shadow.weapon = "{\"mode\":" + TestBench::bodyJson(mode==WeaponState::Fist ? "fist" :
                                                   (mode==WeaponState::W1H ? "1h" : "2h")) +
                  ",\"name\":" + TestBench::bodyJson(weaponSymbol!=nullptr ? std::string_view(weaponSymbol->name()) : std::string_view("unarmed")) +
                  ",\"visual\":" + TestBench::bodyJson(item==nullptr ? "" : item->handle().visual) +
                  ",\"length_cm\":" + std::to_string(length) + "}";
  shadow.legacyTarget = "null";
  shadow.legacyNpc = nullptr;
  shadow.legacyResult = "no_optimal_frame";
  shadow.legacyHit = shadow.legacyBlock = shadow.legacyDodge = false;
  shadow.damage = 0;
  shadow.consumed = shadow.targetsHit = 0;
  shadow.struckVobs.clear();
  shadow.stopped = false;
  refreshMeleeFighter();
  // The previous movement tick can still report a controller after its gates changed.
  shadow.physicsHits = MeleeFighter::physicsHits(Gothic::physicsFlags().meleePhysicsHits,isPlayer(),
                                                !isPlayer() && mvAlgo.characterVirtualReady());
  const auto* hero = owner.player();
  shadow.heroDistanceCm = hero!=nullptr ? std::sqrt(qDistTo(*hero)) : 0.f;
  shadow.outcome = "miss";
  TestBench::npcDuelSwingStart(*this);
  shadow.magnet = {};
  shadow.reachCpuMs = shadow.reachCpuMaxMs = 0;
  shadow.reachQueries = 0;
  shadow.diagnostic = {};
  if(TestBench::bodyTracing() && currentTarget!=nullptr) {
    shadow.diagnostic.target = currentTarget;
    shadow.diagnostic.who = meleeShadowWho(currentTarget);
    shadow.diagnostic.startDistanceCm = fightDistanceTo(*currentTarget).length();
    }
  if(shadow.physicsHits && currentTarget!=nullptr && fghAlgo.isInFocusAngle(*this,*currentTarget,60.f)) {
    auto& magnet = shadow.magnet;
    magnet.eligible = true;
    magnet.target = currentTarget;
    auto direction = currentTarget->position()-position();
    direction.y = 0;
    if(direction.length()>0.f)
      magnet.direction = Vec3::normalize(direction);
    const float yaw = std::remainder(angleDir(direction.x,direction.z)-angle,360.f);
    // Fix the goal at the start of this swing. The active window is unchanged;
    // Planned assistance ends at the preview contact. Only protective backoff
    // may continue afterwards, including when no preview contact was found.
    const auto started = std::chrono::steady_clock::now();
    Pose sampled = pose;
    auto reach = sampleMeleeReach(*currentTarget,sampled,*attack,yaw);
    reach.cpuMs = std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
    owner.physic()->accountMeleeReachTime(reach.cpuMs);
    shadow.reachCpuMs = shadow.reachCpuMaxMs = reach.cpuMs;
    shadow.reachQueries = 1;
    shadow.diagnostic.previewAdvanceCm = reach.advanceCm;
    shadow.diagnostic.previewContactMs = reach.contactMs;
    shadow.diagnostic.previewCpuMs = reach.cpuMs;
    shadow.diagnostic.previewApproachCm = reach.advanceCm;
    magnet.rawCm = reach.advanceCm;
    magnet.previewTorsoGapCm = reach.torsoGapCm;
    if(std::isfinite(reach.advanceCm)) {
      magnet.durationMs = reach.contactMs;
      magnet.yawRequested = reach.yawDeg;
      magnet.feasible = reach.uncoveredCm==0.f;
      const float backward = std::min(40.f,.6f*reach.contactMs);
      const float forward = std::min(120.f,.6f*reach.contactMs);
      magnet.requestedCm = std::clamp(reach.advanceCm,-backward,forward);
      }
    }
  }

bool Npc::wantsMeleeShadowBody() const {
  return !meleeShadowSuspended && (Gothic::physicsFlags().meleeBladeShadow || Gothic::physicsFlags().meleePhysicsHits ||
                                   (meleeShadow && meleeShadow->window));
  }

void Npc::prepareMeleeShadowBody() {
  if(wantsMeleeShadowBody())
    poseMeleeShadowBody();
  }

void Npc::poseMeleeShadowBody() {
  auto& cached = meleeShadowPose;
  const auto& pose = visual.pose();
  const auto& matrix = visual.transform();
  // The scale exactly as MdlVisual::hitZonesPosed computes it.
  const auto scale = std::bit_cast<uint32_t>(Vec3(matrix.at(0,0),matrix.at(0,1),matrix.at(0,2)).length());
  const auto shape = visual.hitZoneShape().revision();
  if(cached.pose==&pose && cached.bones==pose.revision() && cached.shape==shape && cached.scale==scale)
    return;
  visual.hitZonesPosed(cached.zones);
  auto& capsules = cached.body.capsules;
  capsules.clear();
  for(const auto& capsule:cached.zones)
    capsules.push_back({capsule.a,capsule.b,capsule.r,capsule.bone,uint8_t(capsule.zone)});
  DynamicWorld::boundShadowBody(cached.body);
  // Unique across NPCs and frames: a slot holding this serial holds these capsules.
  static std::atomic<uint64_t> serials{0};
  cached.body.serial = serials.fetch_add(1,std::memory_order_relaxed)+1;
  cached.pose  = &pose;
  cached.bones = pose.revision();
  cached.shape = shape;
  cached.scale = scale;
  }

// OPENGOTHIC_MELEE_SHADOW_CHECK=1: after each publication, poses the body again
// the way it was always posed in place and compares it bit by bit with the
// published one. Counts go to stderr every 100000 bodies and on every mismatch.
static void checkMeleeShadowBody(const Npc& npc, const World& world) {
  static const bool enabled = [] {
    const char* v = std::getenv("OPENGOTHIC_MELEE_SHADOW_CHECK");
    return v!=nullptr && v[0]=='1';
    }();
  if(!enabled)
    return;
  static uint64_t checked = 0, mismatched = 0;
  std::vector<HitZones::Posed> zones;
  npc.hitZonesPosed(zones);
  MeleeShadowBody expected, published;
  for(const auto& capsule:zones)
    expected.capsules.push_back({capsule.a,capsule.b,capsule.r,capsule.bone,uint8_t(capsule.zone)});
  DynamicWorld::boundShadowBody(expected);
  const auto bits = [](const Vec3& v) {
    return std::array<uint32_t,3>{std::bit_cast<uint32_t>(v.x),std::bit_cast<uint32_t>(v.y),std::bit_cast<uint32_t>(v.z)};
    };
  bool same = world.physic()->shadowBody(&npc,published) && published.capsules.size()==expected.capsules.size() &&
              bits(published.boundsMin)==bits(expected.boundsMin) && bits(published.boundsMax)==bits(expected.boundsMax);
  for(size_t i=0; same && i<expected.capsules.size(); ++i) {
    const auto& a = published.capsules[i];
    const auto& b = expected.capsules[i];
    same = bits(a.from)==bits(b.from) && bits(a.to)==bits(b.to) &&
           std::bit_cast<uint32_t>(a.radiusCm)==std::bit_cast<uint32_t>(b.radiusCm) && a.bone==b.bone && a.zone==b.zone;
    }
  ++checked;
  if(!same)
    ++mismatched;
  if(!same || checked%100000==0)
    std::fprintf(stderr,"[melee-shadow-check] checked=%llu mismatched=%llu%s%s\n",
                 static_cast<unsigned long long>(checked),static_cast<unsigned long long>(mismatched),
                 same ? "" : " npc=",same ? "" : std::string(npc.displayName()).c_str());
  }

void Npc::updateMeleeShadow() {
  if(!wantsMeleeShadowBody()) {
    clearMeleeShadow();
    return;
    }
  // Usually posed already by the parallel animation pass; reposed here when
  // that pass skipped this NPC or something moved its bones since.
  poseMeleeShadowBody();
  const auto& zones = meleeShadowPose.zones;
  owner.physic()->updateShadowBody(this,meleeShadowPose.body,&meleeShadowPose.slot);
  meleeShadowBody = true;
  checkMeleeShadowBody(*this,owner);
  if(TestBench::bodyTracing() && isHuman() && currentTarget!=nullptr && owner.npcId(currentTarget)!=uint32_t(-1)) {
    std::vector<HitZones::Posed> targetZones;
    currentTarget->visual.hitZonesPosed(targetZones);
    const float gap = HitZones::horizontalBodyGap(zones,targetZones);
    const auto radius = [](const std::vector<HitZones::Posed>& capsules) {
      float largest = 0;
      for(const auto& capsule:capsules)
        if(capsule.zone==HitZones::Body) largest = std::max(largest,capsule.r);
      return largest;
      };
    TestBench::bodyEvent(*this,currentTarget,"melee_torso_gap",owner.tickCount(),
                        "\"gap_cm\":"+(std::isfinite(gap) ? std::to_string(gap) : "null")+
                        ",\"overlap\":"+(gap<0.f ? "true" : "false")+
                        ",\"attacker_down\":"+(isDown() ? "true" : "false")+
                        ",\"target_down\":"+(currentTarget->isDown() ? "true" : "false")+
                        ",\"attacker_body_radius_cm\":"+std::to_string(radius(zones))+
                        ",\"target_body_radius_cm\":"+std::to_string(radius(targetZones)));
    }
  syncMeleeShadowSwing();
  if(!meleeShadow || !meleeShadow->window)
    return;
  auto& shadow = *meleeShadow;
  const auto& pose = visual.pose();
  const auto& window = *shadow.window;
  const auto bonePoint = [&](size_t bone,Vec3 point) {
    pose.bone(bone).project(point);
    return point;
    };
  std::vector<MeleeBladeSegment> blade;
  if(weaponState()==WeaponState::Fist) {
    const auto hand = pose.findNode(window.left ? "BIP01 L HAND" : "BIP01 R HAND");
    const auto forearm = pose.findNode(window.left ? "BIP01 L FOREARM" : "BIP01 R FOREARM");
    if(hand<pose.boneCount() && forearm<pose.boneCount()) {
      const auto fist = bonePoint(hand,{});
      const auto elbow = bonePoint(forearm,{});
      auto normal = bonePoint(hand,{0,1,0})-fist;
      const auto finger = pose.findNode(window.left ? "BIP01 L FINGER0" : "BIP01 R FINGER0");
      const auto knuckle = finger<pose.boneCount() ? bonePoint(finger,{}) : fist;
      blade.push_back({fist,knuckle,normal,6.f});
      blade.push_back({elbow,fist,normal,4.f});
      }
    }
  else {
    const auto hand = pose.findNode("ZS_RIGHTHAND");
    if(hand<pose.boneCount()) {
      const auto grip = bonePoint(hand,{});
      const Vec3 normal = window.normalY ? Vec3(0,1,0) : Vec3(0,0,1);
      blade.push_back({grip,bonePoint(hand,{-window.length,0,0}),bonePoint(hand,normal)-grip,2.f});
      }
    }
  // Publish the whole pose interval. Physics clips it to the authored window,
  // including frames whose endpoint is already past the end of that window.
  const double activeBegin = double(shadow.start)+double(window.beginMs)/shadow.speed;
  const double activeEnd   = double(shadow.start)+double(window.endMs)/shadow.speed;
  if(TestBench::bodyTracing() && double(owner.tickCount())>=activeBegin && double(owner.tickCount())<activeEnd) {
    auto& diagnostic = shadow.diagnostic;
    if(const auto* target = diagnostic.target; target!=nullptr && owner.npcId(target)!=uint32_t(-1)) {
      std::vector<HitZones::Posed> targetZones;
      target->visual.hitZonesPosed(targetZones);
      const auto root = position();
      if(!diagnostic.sampled && !targetZones.empty()) {
        diagnostic.sampled = true;
        diagnostic.sample = owner.tickCount();
        diagnostic.targetStart = target->position();
        auto direction = target->position()-root;
        direction.y = 0;
        diagnostic.forward = direction.length()>0 ? Vec3::normalize(direction) : Vec3();
        float distance = std::numeric_limits<float>::max();
        for(const auto& zone:targetZones) {
          const Vec2 a(zone.a.x-root.x,zone.a.z-root.z), b(zone.b.x-root.x,zone.b.z-root.z);
          const auto axis = b-a;
          const float at = axis.quadLength()>0 ? std::clamp(-(a.x*axis.x+a.y*axis.y)/axis.quadLength(),0.f,1.f) : 0.f;
          distance = std::min(distance,(a+axis*at).length()-zone.r);
          }
        diagnostic.initialGapCm = distance-window.reachCm;
        }
      // a melee dodge is no jump back: a blow on it outside its window is not an evasion
      diagnostic.jumpback |= !target->isMeleeDodging() && target->visual.pose().isJumpBack(owner.tickCount());
      diagnostic.retreatCm = std::max(diagnostic.retreatCm,Vec3::dotProduct(target->position()-diagnostic.targetStart,diagnostic.forward));
      // Closest points on the two capsule axes. This tracing-only summary uses
      // actual published poses; it never changes the collision query or damage.
      for(const auto& segment:blade)
        for(const auto& zone:targetZones) {
          const auto u = segment.to-segment.from, v = zone.b-zone.a, r = segment.from-zone.a;
          const float a = Vec3::dotProduct(u,u), e = Vec3::dotProduct(v,v), f = Vec3::dotProduct(v,r);
          float s = 0, t = 0;
          if(a<=1.e-6f) {
            t = e>1.e-6f ? std::clamp(f/e,0.f,1.f) : 0.f;
            }
          else {
            const float c = Vec3::dotProduct(u,r);
            if(e<=1.e-6f) s = std::clamp(-c/a,0.f,1.f);
            else {
              const float b = Vec3::dotProduct(u,v), denominator = a*e-b*b;
              s = denominator>1.e-6f ? std::clamp((b*f-c*e)/denominator,0.f,1.f) : 0.f;
              t = (b*s+f)/e;
              if(t<0) { t = 0; s = std::clamp(-c/a,0.f,1.f); }
              else if(t>1) { t = 1; s = std::clamp((b-c)/a,0.f,1.f); }
              }
            }
          const auto offset = segment.from+u*s-(zone.a+v*t);
          const float clearance = offset.length()-segment.radiusCm-zone.r;
          if(clearance<diagnostic.minClearanceCm) {
            diagnostic.minClearanceCm = clearance;
            const auto side = Vec3(-diagnostic.forward.z,0,diagnostic.forward.x);
            diagnostic.nearestOffset = {Vec3::dotProduct(offset,diagnostic.forward),offset.y,Vec3::dotProduct(offset,side)};
            }
          }
      }
    }
  if(shadow.stopped)
    return;
  const float walkableCos = std::cos(float(owner.script().guildVal().slide_angle[guild()])*float(M_PI)/180.f);
  const auto weapon=weaponState()==WeaponState::Fist ? MeleeImpact::Weapon::Fist :
                    weaponState()==WeaponState::W2H ? MeleeImpact::Weapon::TwoHand : MeleeImpact::Weapon::OneHand;
  owner.physic()->updateShadowBlade(this,shadow.id,blade,owner.tickCount(),activeBegin,activeEnd,walkableCos,
                                    Vec3(std::cos(rotationRad()),0,std::sin(rotationRad())),weapon);
  if(Gothic::inst().doMeleeBladeDraw()) {
    const double now = double(owner.tickCount());
    meleeShadowDrawPose   = blade;
    meleeShadowDrawActive = now>=activeBegin && now<activeEnd;
    }
  }

void Npc::resolveMeleeContacts() {
  if(!meleeShadow || !meleeShadow->window)
    return;
  auto contacts = owner.physic()->takeShadowBladeContacts(this);
  noteMeleeShadowMarks(contacts);
  for(const auto& hit:contacts)
    meleeShadow->contacts.push_back({hit,meleeShadowWho(hit.npc),meleeShadowBone(hit)});
  consumeMeleeContacts();
  }

auto Npc::meleeStep(uint64_t dt) -> Vec3 {
  if(!meleeShadow || !meleeShadow->window || !meleeShadow->physicsHits || meleeShadow->stopped)
    return {};
  auto& swing = *meleeShadow;
  auto& magnet = swing.magnet;
  if(!magnet.eligible || dt==0)
    return {};
  if(magnet.target==nullptr || owner.npcId(magnet.target)==uint32_t(-1) || magnet.target->isDown()) {
    magnet.forwardStopped = true;
    return {};
    }
  const double begin = std::max(double(owner.tickCount())-double(dt),double(swing.tick));
  const float milliseconds = float(double(owner.tickCount())-begin);
  if(milliseconds<=0.f)
    return {};
  // Forward feed and turn keep their frozen deadline. Backoff alone can protect
  // the rest of this swing from an approaching target or native attack root.
  const double deadline = double(swing.tick)+magnet.durationMs;
  const double end = std::min(double(owner.tickCount()),deadline);
  const float plannedMs = !magnet.contactStopped && magnet.durationMs>0.f ? std::max(0.f,float(end-begin)) : 0.f;
  float yawStep = 0, distance = 0;
  if(plannedMs>0.f) {
    const float fraction = std::clamp(float(end-double(swing.tick))/magnet.durationMs,0.f,1.f);
    const float yawLeft = magnet.yawRequested*fraction-magnet.yawApplied;
    yawStep = std::clamp(yawLeft,-.18f*plannedMs,.18f*plannedMs);
    if(meleeAiSpecial==MeleeAi::Special::Charge && isMeleeChargeReleasing())
      yawStep = 0.f;
    setDirection(angle+yawStep);
    magnet.yawApplied += yawStep;
    magnet.peakYawDegS = std::max(magnet.peakYawDegS,std::abs(yawStep)*1000.f/float(dt));
    const float left = magnet.requestedCm*fraction-magnet.offeredCm;
    distance = std::clamp(left,-.6f*plannedMs,.6f*plannedMs);
    }
  const auto started = std::chrono::steady_clock::now();
  const auto posedAtRoot = [](const Npc& npc) {
    std::vector<HitZones::Posed> capsules;
    npc.visual.hitZonesPosed(capsules);
    // Bones still contain the previous published transform during movement.
    // Rebase to the live root, including an opponent that moved earlier this tick.
    auto inverse = npc.visual.transform();
    inverse.inverse();
    const auto current = npc.mkPositionMatrix();
    for(auto& capsule:capsules) {
      inverse.project(capsule.a); inverse.project(capsule.b);
      current.project(capsule.a); current.project(capsule.b);
      }
    return capsules;
    };
  const auto torso = posedAtRoot(*this), targetTorso = posedAtRoot(*magnet.target);
  const float gap = HitZones::horizontalBodyGap(torso,targetTorso);
  magnet.minTorsoGapCm = std::min(magnet.minTorsoGapCm,gap);
  ++magnet.safetyFrames;
  const float planned = distance;
  if(!std::isfinite(gap)) {
    magnet.forwardStopped = true;
    distance = 0;
    }
  else {
    const float forward = std::max(0.f,gap-meleeMagnetTorsoGapCm);
    if(distance>0.f) {
      // Once the feed reaches its safety boundary, do not chase a retreating target.
      distance = magnet.forwardStopped ? 0.f : std::min(distance,forward);
      if(distance<planned)
        magnet.forwardStopped = true;
      }
    if(gap<=meleeMagnetTorsoGapCm)
      magnet.forwardStopped = true;
    if(gap<meleeMagnetTorsoGapCm)
      distance = std::min(distance,-std::min(meleeMagnetTorsoGapCm-gap,.6f*milliseconds));
    distance = std::max(distance,-std::max(0.f,40.f-magnet.backwardCm));
    if(distance<0.f) {
      const float firstGap = HitZones::horizontalBodyGap(torso,targetTorso,magnet.direction*-std::min(1.f,-distance));
      const float lastGap = HitZones::horizontalBodyGap(torso,targetTorso,magnet.direction*distance);
      // Intersecting projected axes have a flat distance until they separate.
      // Permit that escape plateau, but never move towards a smaller clearance.
      if(firstGap<gap || lastGap<gap)
        distance = 0;
      }
    }
  const float after = HitZones::horizontalBodyGap(torso,targetTorso,magnet.direction*distance);
  if(distance!=planned)
    ++magnet.safetyClamps;
  if(distance<planned && distance<0.f)
    magnet.safetyBackoffCm += -distance;
  if(distance>0.f) magnet.forwardCm += distance;
  else magnet.backwardCm -= distance;
  if(TestBench::bodyTracing())
    TestBench::bodyEvent(*this,magnet.target,"melee_magnet_step",owner.tickCount(),
                        "\"swing\":"+std::to_string(swing.id)+",\"dt_ms\":"+std::to_string(dt)+
                        ",\"planned_ms\":"+std::to_string(plannedMs)+",\"yaw_step_deg\":"+std::to_string(yawStep)+
                        ",\"gap_cm\":"+(std::isfinite(gap) ? std::to_string(gap) : "null")+
                        ",\"after_command_gap_cm\":"+(std::isfinite(after) ? std::to_string(after) : "null")+
                        ",\"planned_cm\":"+std::to_string(planned)+",\"offered_cm\":"+std::to_string(distance)+
                        ",\"backward_cm\":"+std::to_string(magnet.backwardCm)+
                        ",\"forward_stopped\":"+(magnet.forwardStopped ? "true" : "false"));
  owner.physic()->accountMeleeReachTime(std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count());
  magnet.offeredCm += distance;
  magnet.peakCmS = std::max(magnet.peakCmS,std::abs(distance)*1000.f/float(dt));
  // This timestamp retains the deadline of planned assistance; protective
  // backoff has its own per-frame trace and may outlive that deadline.
  if(plannedMs>0.f && (distance>0.f || (distance==planned && distance<0.f) || yawStep!=0.f))
    magnet.lastFeedMs = end;
  return magnet.direction*distance;
  }

void Npc::noteMeleeStep(const Vec3& root, const Vec3& offered, const Vec3& moved, uint64_t dt) {
  if(!meleeShadow || offered.quadLength()==0.f || dt==0)
    return;
  auto& magnet = meleeShadow->magnet;
  // One CV solve owns root and assistance together. Record that accepted
  // projection separately; root subtraction is a conservative attribution,
  // not a second, hypothetical solve with assistance removed.
  const auto direction = Vec3::normalize(offered);
  const float accepted = Vec3::dotProduct(moved,direction);
  const float estimated = std::clamp(accepted-std::max(0.f,Vec3::dotProduct(root,direction)),0.f,offered.length());
  const float sign = Vec3::dotProduct(offered,magnet.direction)<0.f ? -1.f : 1.f;
  magnet.combinedCm += sign*accepted;
  magnet.estimatedCm += sign*estimated;
  magnet.estimatedPeakCmS = std::max(magnet.estimatedPeakCmS,estimated*1000.f/float(dt));
  }

void Npc::stopMeleeSwing() {
  if(meleeChargePhase==ChargePhase::Release) {
    meleeChargePhase = ChargePhase::None;
    meleeChargeMult = 1.f;
    }
  owner.physic()->removeShadowBlade(this);
  // The weapon rebounds, the body stays: the blow is cut off and blends back into the fight
  // stance, both hands on the grip. A stumble here (a hit taken) threw the body back and
  // dropped a two-hander to one hand at every post.
  visual.interrupt();
  setAnim(Anim::Idle);
  }

void Npc::consumeMeleeContacts() {
  if(!meleeShadow || !meleeShadow->window)
    return;
  auto& swing = *meleeShadow;
  while(swing.consumed<swing.contacts.size()) {
    const auto index = swing.consumed++;
    const auto id = swing.id;
    auto& entry = swing.contacts[index];
    const auto hit = entry.hit;
    // Walkable ground is a logged brush, not the target contact ending assistance.
    if(swing.magnet.eligible && !swing.magnet.contactStopped && (!hit.world || hit.stopsSwing)) {
      swing.magnet.contactStopped = true;
      swing.magnet.contactObservedMs = owner.tickCount();
      swing.magnet.contactKind = hit.world ? "wall" : "body";
      }
    if(swing.stopped) {
      entry.result = "after_stop";
      continue;
      }
    if(hit.world) {
      entry.result = hit.stopsSwing ? "wall" : (hit.windup ? "windup" : "ground");
      if(!hit.stopsSwing)
        continue;
      swing.stopped = true;
      swing.outcome = "wall";
      if(swing.physicsHits) {
        const auto* weapon = inventory().activeWeapon();
        Matrix4x4 at;
        at.identity();
        at.translate(hit.point);
        owner.addLandHitEffect(weapon!=nullptr ? ItemMaterial(weapon->handle().material) : ItemMaterial::MAT_LEATHER,hit.material,at).play();
        // The blade bites into an intact board instead of passing it, and the board
        // takes this blow: the hit frame that applies destruction never comes now.
        MeleeDestructionBlow blow;
        if((hit.movableVob!=0 || hit.fragment!=0) && owner.physic()->destructionEnabled() && meleeDestructionBlow(blow)) {
          const bool struck = owner.physic()->meleeStoppedMovableVob(hit,blow.weaponDamage,blow.strength,blow.kind,
                                                                     this,swing.struckVobs);
          if(TestBench::isActive())
            TestBench::destructionMeleeHit(*this,struck ? 1 : 0,blow.weaponDamage+blow.strength);
          }
        stopMeleeSwing();
        }
      continue;
      }
    auto* target = hit.npc;
    if(target==nullptr || target->isDown()) {
      entry.result = "target_down";
      continue;
      }
    const int partyMember = 15;
    const bool sameParty = owner.script().isFriendlyFire(*this,*target) ||
                           (handle().aivar[partyMember]!=0 && target->handle().aivar[partyMember]!=0);
    if(skipMeleeAlly(isPlayer(),target==currentTarget,guild()==target->guild(),sameParty,isEnemy(*target))) {
      entry.result = "ally_skipped";
      if(swing.outcome=="miss")
        swing.outcome = "ally_skipped";
      continue;
      }
    const auto& targetPose = target->visual.pose();
    // A melee dodge is neither the jump back nor a parade: inside its window the blade passes (takeDamage logs
    // it), outside the window it hits
    const bool dodging = target->isMeleeDodging();
    const bool dodge = (dodging && target->isMeleeDodgeInvulnerable()) ||
                       (!dodging && !target->meleeKnockdown && !target->isMeleeStunned() && targetPose.isJumpBack(owner.tickCount()) && target->fghAlgo.isInJumpBackAngle(*target,*this));
    const bool block = !dodging && !target->meleeKnockdown && !target->isMeleeStunned() && target->fghAlgo.isInFocusAngle(*target,*this) && targetPose.isDefence(owner.tickCount());
    entry.result = block ? "blocked" : (dodge ? "evaded" : "hit");
    entry.chargeMult = meleeChargeMultiplier();
    if(!block && !dodge) {
      const auto backstab = target->meleeBackstabKind(*this);
      entry.backstabMult = target->meleeBackstabMultiplier(backstab);
      entry.stunBonus = backstab==MeleeImpact::BackstabKind::None ? target->meleeStunBonus(*this) : 1.f;
      entry.superArmor = target->hasMeleeSuperArmor();
      // The sampled MAN peak is fixed per animation/window, not learned from outcomes.
      entry.multiplier = DamageCalculator::meleeMultiplier(hit.speedCmS,swing.window->peakCmS*.7f,
                                                          hit.bladeFraction,hit.surfaceAngleDeg,swing.targetsHit++);
      swing.outcome = "hit";
      }
    if(swing.physicsHits) {
      BodyObservations::noteEvent(*this,target,"applied",owner.tickCount());
      if(TestBench::bodyTracing())
        TestBench::bodyEvent(*this,target,"hit_attempt",owner.tickCount(),
                            "\"result\":"+TestBench::bodyJson(entry.result),visual.pose().animNames());
      const auto before = target->attribute(ATR_HITPOINTS);
      target->takeDamage(*this,nullptr,false,entry.multiplier,&hit);
      // Damage scripts may remove the attacker, retiring and clearing this window.
      if(!swing.window || swing.id!=id)
        return;
      swing.contacts[index].damage = std::max(0,before-target->attribute(ATR_HITPOINTS));
      if(!dodge)
        meleeHitStop(*target,hit,entry.multiplier,block);
      }
    if(block) {
      swing.stopped = true;
      swing.outcome = "blocked";
      if(swing.physicsHits)
        stopMeleeSwing();
      }
    }
  }

void Npc::meleeHitStop(Npc& target, const MeleeBladeContact& hit, float multiplier, bool blocked) {
  if(!Gothic::physicsFlags().meleeHitStop)
    return;
  // The blade bites and both bodies hold their frame for a moment, then go on: the weight of the
  // blow. A blocked one holds only the guard, the blow itself rebounds into the stance.
  const auto ws = weaponState();
  // Under three frames of 30 the hold is not seen at all: a weak blow still holds most of it
  float ms = ws==WeaponState::W2H ? 120.f : (ws==WeaponState::W1H ? 90.f : 60.f);
  ms = blocked ? 60.f : ms*std::clamp(multiplier,.8f,1.3f);
  constexpr float hold = .04f;
  const uint64_t until = owner.tickCount()+uint64_t(ms);
  if(!blocked)
    visual.hitStop(until,hold);
  target.visual.hitStop(until,hold);
  if(isPlayer() || target.isPlayer()) {
    if(auto camera = Gothic::inst().camera()) {
      const float deg = target.isPlayer() ? (blocked ? .5f : 1.2f) : (blocked ? .3f : .5f);
      camera->impact(hit.velocityCmS,deg*(blocked ? 1.f : std::clamp(multiplier,.8f,1.3f)));
      }
    }
  if(TestBench::bodyTracing())
    TestBench::bodyEvent(*this,&target,"hit_stop",owner.tickCount(),
                        "\"ms\":"+std::to_string(int(ms))+",\"blocked\":"+(blocked ? "true" : "false"),visual.pose().animNames());
  }

void Npc::noteMeleeShadowMarks(const std::vector<MeleeBladeContact>& contacts) {
  if(!Gothic::inst().doMeleeBladeDraw())
    return;
  const uint64_t now = owner.tickCount();
  for(const auto& hit:contacts) {
    std::string label = hit.world ? std::string("world") : std::string(HitZones::name(HitZones::Zone(hit.zone)));
    if(!hit.world)
      label += " " + std::to_string(int(hit.speedCmS)) + "cm/s";
    meleeShadowMarks.push_back({hit.point,now,hit.world,std::move(label)});
    }
  constexpr size_t maxMarks = 16;
  if(meleeShadowMarks.size()>maxMarks)
    meleeShadowMarks.erase(meleeShadowMarks.begin(),meleeShadowMarks.end()-maxMarks);
  }

void Npc::drawMeleeShadow(DbgPainter& p, uint64_t now) const {
  constexpr uint64_t markLifetime = 3000;
  if(meleeShadow && meleeShadow->window) {
    // Yellow while the blade can hit, grey before and after the active window.
    p.setPen(meleeShadowDrawActive ? Tempest::Color(1,1,0) : Tempest::Color(0.6f,0.6f,0.6f));
    for(const auto& segment:meleeShadowDrawPose) {
      p.drawLine(segment.from,segment.to);
      p.drawPoint(segment.to,3);
      }
    }
  for(const auto& mark:meleeShadowMarks) {
    if(now>mark.time+markLifetime)
      continue;
    // Red: a body part was hit; blue: the world stopped the swing.
    p.setBrush(mark.world ? Tempest::Color(0.2f,0.4f,1) : Tempest::Color(1,0,0));
    p.setPen(mark.world ? Tempest::Color(0.2f,0.4f,1) : Tempest::Color(1,0,0));
    p.drawPoint(mark.point,6);
    if(Gothic::inst().doMeleeBladeLabels())
      p.drawText(mark.point,mark.label);
    }
  }

void Npc::clearMeleeShadow() {
  finishMeleeShadowSwing(true,true);
  if(meleeShadowBody && owner.physic()!=nullptr) {
    owner.physic()->removeShadowBody(this);
    meleeShadowBody = false;
    }
  }

void Npc::finishMeleeShadowSwing(bool remove, bool discard) {
  if(!meleeShadow || !meleeShadow->window || owner.physic()==nullptr)
    return;
  auto& shadow = *meleeShadow;
  auto contacts = owner.physic()->takeShadowBladeContacts(this);
  noteMeleeShadowMarks(contacts);
  meleeShadowDrawPose.clear();
  meleeShadowDrawActive = false;
  for(const auto& hit:contacts)
    shadow.contacts.push_back({hit,meleeShadowWho(hit.npc),meleeShadowBone(hit)});
  const auto id = shadow.id;
  if(discard) {
    if(shadow.consumed<shadow.contacts.size() && shadow.outcome=="miss")
      shadow.outcome = "discarded";
    while(shadow.consumed<shadow.contacts.size())
      shadow.contacts[shadow.consumed++].result = "discarded";
    }
  else consumeMeleeContacts();
  // A nested removal already wrote and cleared the window; do not flush it twice.
  if(!shadow.window || shadow.id!=id)
    return;
  if(remove)
    owner.physic()->removeShadowBlade(this);
  const auto& window = *shadow.window;
  bool physicalHit = false, sameTarget = false, extraTarget = false, worldHit = false;
  std::string physical = "[", world = "null", worldContacts = "[";
  for(const auto& entry:shadow.contacts) {
    const auto& contact = entry.hit;
    const auto point = meleeShadowPoint(contact.point);
    if(contact.world) {
      worldHit |= entry.result=="wall";
      world = "{\"point_cm\":" + point + ",\"normal\":" + meleeShadowPoint(contact.normal) +
              ",\"velocity_cm_s\":" + meleeShadowPoint(contact.velocityCmS) +
              ",\"material\":" + std::to_string(int(contact.material)) +
              ",\"result\":" + TestBench::bodyJson(entry.result) + ",\"step\":" + std::to_string(contact.step) + "}";
      if(worldContacts.size()>1)
        worldContacts += ',';
      worldContacts += world;
      continue;
      }
    if(entry.result=="hit")
      physicalHit = true;
    const bool same = contact.npc!=nullptr && contact.npc==shadow.legacyNpc;
    sameTarget |= same && entry.result=="hit";
    extraTarget |= !same && entry.result=="hit";
    if(physical.size()>1)
      physical += ',';
    physical += "{\"result\":" + TestBench::bodyJson(entry.result) +
                ",\"multiplier\":" + std::to_string(entry.multiplier) + ",\"damage\":" + std::to_string(entry.damage) +
                ",\"stun_bonus\":" + std::to_string(entry.stunBonus) +
                ",\"backstab_mult\":" + std::to_string(entry.backstabMult) +
                ",\"charge_mult\":" + std::to_string(entry.chargeMult) +
                ",\"riposte_mult\":1" +
                ",\"super_armor\":" + (entry.superArmor ? "true" : "false") +
                ",\"target\":" + entry.target +
                ",\"part\":" + TestBench::bodyJson(HitZones::name(HitZones::Zone(contact.zone))) +
                ",\"bone\":" + std::to_string(contact.bone) + ",\"bone_name\":" + TestBench::bodyJson(entry.bone) +
                ",\"point_cm\":" + point +
                ",\"blade_fraction\":" + std::to_string(contact.bladeFraction) +
                ",\"speed_cm_s\":" + std::to_string(contact.speedCmS) +
                ",\"surface_angle_deg\":" + std::to_string(contact.surfaceAngleDeg) +
                ",\"step\":" + std::to_string(contact.step) + "}";
    }
  physical += ']';
  worldContacts += ']';
  std::string comparison = "null";
  if(worldHit && shadow.legacyHit && !sameTarget)
    comparison = "\"world_before_target\"";
  else if(physicalHit && (extraTarget || !sameTarget) && shadow.legacyHit)
    comparison = "\"different_or_extra_target\"";
  else if(physicalHit && shadow.legacyHit)
    comparison = "\"same_target\"";
  else if(physicalHit)
    comparison = "\"physics_only\"";
  else if(shadow.legacyHit)
    comparison = "\"legacy_only\"";
  const auto number = [](auto value) { return std::to_string(value); };
  std::string diagnostic = "null";
  if(TestBench::bodyTracing()) {
    const auto& value = shadow.diagnostic;
    const double activeBegin = double(shadow.start)+double(window.beginMs)/shadow.speed;
    const double activeEnd = double(shadow.start)+double(window.endMs)/shadow.speed;
    diagnostic = "{\"target\":" + value.who +
                 ",\"start_distance_cm\":" + number(value.startDistanceCm) +
                 ",\"active_begin_ms\":" + number(activeBegin) +
                 ",\"active_end_ms\":" + number(activeEnd) +
                 ",\"sample_time_ms\":" + number(value.sample) +
                 ",\"sampled\":" + (value.sampled ? "true" : "false") +
                 ",\"reach_cm\":" + number(window.reachCm) +
                 ",\"preview_advance_cm\":" + (std::isfinite(value.previewAdvanceCm) ? number(value.previewAdvanceCm) : "null") +
                 ",\"preview_contact_ms\":" + number(value.previewContactMs) +
                 ",\"preview_approach_cm\":" + (std::isfinite(value.previewApproachCm) ? number(value.previewApproachCm) : "null") +
                 ",\"preview_cpu_ms\":" + number(value.previewCpuMs) +
                 ",\"reach_queries\":" + number(shadow.reachQueries) +
                 ",\"reach_cpu_total_ms\":" + number(shadow.reachCpuMs) +
                 ",\"reach_cpu_max_ms\":" + number(shadow.reachCpuMaxMs) +
                 ",\"initial_gap_cm\":" + (value.sampled ? number(value.initialGapCm) : "null") +
                 ",\"min_clearance_cm\":" + (value.minClearanceCm<std::numeric_limits<float>::max() ? number(value.minClearanceCm) : "null") +
                 ",\"nearest_offset_forward_up_side_cm\":" + meleeShadowPoint(value.nearestOffset) +
                 ",\"target_retreat_cm\":" + number(value.retreatCm) +
                 ",\"jumpback\":" + (value.jumpback ? "true" : "false") +
                 ",\"window_completed\":" + (double(owner.tickCount())>=activeEnd ? "true" : "false") + "}";
    }
  const auto& magnet = shadow.magnet;
  const double deadline = double(shadow.tick)+magnet.durationMs;
  const char* magnetStop = "none";
  if(magnet.eligible) {
    if(magnet.contactStopped && (!std::isfinite(magnet.rawCm) || double(magnet.contactObservedMs)<=deadline))
      magnetStop = magnet.contactKind;
    else if(!std::isfinite(magnet.rawCm))
      magnetStop = "no_preview";
    else
      magnetStop = double(owner.tickCount())>=deadline ? "predicted" : "swing_end";
    }
  writeMeleeShadow("{\"swing\":" + number(shadow.id) + ",\"world\":" + TestBench::bodyJson(owner.name()) +
                  ",\"game_time_ms\":" + number(shadow.tick) + ",\"end_time_ms\":" + number(owner.tickCount()) +
                  ",\"attacker\":" + shadow.attacker + ",\"weapon\":" + shadow.weapon +
                  ",\"animation\":" + TestBench::bodyJson(shadow.animation) +
                  ",\"window\":{\"combo_index\":" + number(window.index) +
                  ",\"first_frame\":" + number(window.first) + ",\"optimal_frame\":" + number(window.optimal) +
                  ",\"last_frame\":" + number(window.last) + ",\"begin_ms\":" + number(window.beginMs) +
                  ",\"optimal_ms\":" + number(window.optimalMs) + ",\"end_ms\":" + number(window.endMs) +
                  ",\"event_boundary_ms\":" + number(window.eventBoundaryMs) +
                  ",\"reverse\":" + (window.sequence->reverse ? "true" : "false") +
                  ",\"animation_speed\":" + number(shadow.speed) + ",\"peak_cm_s\":" + number(window.peakCmS) +
                  ",\"threshold_cm_s\":" + number(window.thresholdCmS) + "},\"legacy\":{\"target\":" +
                  shadow.legacyTarget + ",\"hit\":" + (shadow.legacyHit ? "true" : "false") +
                  ",\"damage\":" + number(shadow.damage) + ",\"blocked\":" + (shadow.legacyBlock ? "true" : "false") +
                  ",\"dodged\":" + (shadow.legacyDodge ? "true" : "false") +
                  ",\"result\":" + TestBench::bodyJson(shadow.legacyResult) +
                  "},\"physical\":{\"targets\":" + physical + ",\"world\":" + world + ",\"world_contacts\":" + worldContacts +
                  "},\"decision\":{\"physics_hits\":" + (shadow.physicsHits ? "true" : "false") +
                  ",\"hero_distance_cm\":" + number(shadow.heroDistanceCm) +
                  ",\"outcome\":" + TestBench::bodyJson(shadow.outcome) +
                  ",\"magnet\":{\"eligible\":" + (shadow.magnet.eligible ? "true" : "false") +
                  ",\"preview_contact\":" + (std::isfinite(shadow.magnet.rawCm) ? "true" : "false") +
                  ",\"raw_cm\":" + (std::isfinite(shadow.magnet.rawCm) ? number(shadow.magnet.rawCm) : "null") +
                  ",\"requested_cm\":" + number(shadow.magnet.requestedCm) +
                  ",\"offered_cm\":" + number(shadow.magnet.offeredCm) +
                  ",\"forward_cm\":" + number(shadow.magnet.forwardCm) +
                  ",\"backward_cm\":" + number(shadow.magnet.backwardCm) +
                  ",\"safety_backoff_cm\":" + number(shadow.magnet.safetyBackoffCm) +
                  ",\"safety_frames\":" + number(shadow.magnet.safetyFrames) +
                  ",\"safety_clamps\":" + number(shadow.magnet.safetyClamps) +
                  ",\"forward_stopped\":" + (shadow.magnet.forwardStopped ? "true" : "false") +
                  ",\"min_torso_gap_cm\":" + (std::isfinite(shadow.magnet.minTorsoGapCm) ? number(shadow.magnet.minTorsoGapCm) : "null") +
                  ",\"preview_torso_gap_cm\":" + (std::isfinite(shadow.magnet.previewTorsoGapCm) ? number(shadow.magnet.previewTorsoGapCm) : "null") +
                  ",\"accepted_combined_cm\":" + number(shadow.magnet.combinedCm) +
                  ",\"estimated_moved_cm\":" + number(shadow.magnet.estimatedCm) +
                  ",\"estimate_method\":\"accepted_projection_minus_forward_root\"" +
                  ",\"peak_speed_cm_s\":" + number(shadow.magnet.peakCmS) +
                  ",\"estimated_peak_speed_cm_s\":" + number(shadow.magnet.estimatedPeakCmS) +
                  ",\"duration_ms\":" + number(shadow.magnet.durationMs) +
                  ",\"predicted_contact_time_ms\":" + (std::isfinite(shadow.magnet.rawCm) ? number(double(shadow.tick)+shadow.magnet.durationMs) : "null") +
                  ",\"contact_observed_time_ms\":" + (shadow.magnet.contactStopped ? number(shadow.magnet.contactObservedMs) : "null") +
                  ",\"last_assist_time_ms\":" + (shadow.magnet.lastFeedMs>0 ? number(shadow.magnet.lastFeedMs) : "null") +
                  ",\"stop_reason\":" + TestBench::bodyJson(magnetStop) +
                  ",\"preview_feasible\":" + (shadow.magnet.feasible ? "true" : "false") +
                  ",\"time_limited\":" + (std::isfinite(shadow.magnet.rawCm) && std::abs(shadow.magnet.rawCm)>.6f*shadow.magnet.durationMs ? "true" : "false") +
                  ",\"yaw_requested_deg\":" + number(shadow.magnet.yawRequested) +
                  ",\"yaw_applied_deg\":" + number(shadow.magnet.yawApplied) +
                  ",\"peak_yaw_deg_s\":" + number(shadow.magnet.peakYawDegS) +
                  ",\"cap_forward\":" + (shadow.magnet.rawCm>=120.f && std::isfinite(shadow.magnet.rawCm) ? "true" : "false") +
                  ",\"cap_backward\":" + (shadow.magnet.rawCm<=-40.f ? "true" : "false") + "}" +
                  "},\"diagnostic\":" + diagnostic + ",\"comparison\":" + comparison + "}");
  TestBench::npcDuelSwingEnd(*this,shadow.physicsHits ? shadow.outcome : shadow.legacyResult);
  shadow.contacts.clear();
  shadow.window.reset();
  }
