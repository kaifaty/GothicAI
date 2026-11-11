#include "dynamicworld_internal.h"

#include "utils/dbgpainter.h"

namespace {
bool isNpcMoveSkipped(const Tempest::Vec3& displacement) {
  constexpr float eps = 2.f; // Legacy component-wise two centimetre skip.
  return std::abs(displacement.x)<eps && std::abs(displacement.y)<eps && std::abs(displacement.z)<eps;
  }
}

DynamicWorld::NpcItem::~NpcItem() {
  if(owner==nullptr || obj==nullptr)
    return;
  owner->npcList->del(obj);
  delete obj;
  }

void DynamicWorld::NpcItem::setPosition(const Tempest::Vec3& pos) {
  if(obj) {
    resetVirtual();
    implSetPosition(pos);
    owner->npcList->onMove(*obj);
    //NOTE: not very usefull, but can create non-trivial cases, for spells
    // owner->bulletList->onMoveNpc(*obj,*owner->npcList);
    }
  }

void DynamicWorld::NpcItem::implSetPosition(const Tempest::Vec3& pos) {
  obj->setPosition(pos);
  }

void DynamicWorld::NpcItem::setEnable(bool e) {
  if(obj==nullptr || obj->enable==e)
    return;
  obj->enable = e;
  resetVirtual();
  owner->world->touchAabbs();
  }

bool DynamicWorld::NpcItem::isEnabled() const {
  return obj!=nullptr && obj->enable;
  }

void DynamicWorld::NpcItem::setUserPointer(void *p) {
  obj->userPointer = p;
  }

auto DynamicWorld::NpcItem::center() const -> Tempest::Vec3 {
  return obj!=nullptr ? obj->centerPosition() : Tempest::Vec3();
  }

float DynamicWorld::NpcItem::centerY() const {
  return center().y;
  }

auto DynamicWorld::NpcItem::centerAsym() const -> Tempest::Vec3 {
  const float acos = std::cos(obj->angle), asin = std::sin(obj->angle);
  return obj->offsetCenter(acos, asin);
  }

float DynamicWorld::NpcItem::groundOffset() const {
  return obj->groundOffset();
  }

float DynamicWorld::NpcItem::radiusXZ() const {
  return obj->maxRXZ;
  }

float DynamicWorld::NpcItem::capsuleRadius() const {
  return obj!=nullptr ? obj->radius : 0.f;
  }

float DynamicWorld::NpcItem::capsuleHeight() const {
  return obj!=nullptr ? obj->cylinderHeight+2.f*obj->radius : 0.f;
  }

const Tempest::Vec3& DynamicWorld::NpcItem::position() const {
  return obj->pos;
  }

void DynamicWorld::NpcItem::setRotation(float a) {
  obj->angle = float(a*M_PI)/180.f - float(M_PI_2);
  }

void DynamicWorld::NpcItem::setScale(const Tempest::Vec3&) {
  // NOP for now
  }

void DynamicWorld::NpcItem::debugDraw(DbgPainter& p) const {
  const auto extent = Tempest::Vec3(obj->radius, obj->cylinderHeight*0.5f+obj->radius, obj->radius);
  const auto center = obj->centerPosition();
  p.setPen(Tempest::Color(1,0.5f,0));
  p.drawAabb(center-extent,center+extent);
  /*
  auto tr = Tempest::Matrix4x4::mkIdentity();
  tr.translate(obj->pos);
  tr.rotateOY(-float(obj->angle*180.f/M_PI));

  p.setPen(Tempest::Color(1,0,1));
  p.drawObb(tr, obj->bbox);

  auto& a = *obj;
  const float acos = std::cos(a.angle), asin = std::sin(a.angle);
  const auto apos = a.pos + a.offsetCenter(acos, asin);
  p.drawPoint(apos, 10);
  */
  }

bool DynamicWorld::NpcItem::testMove(const Tempest::Vec3& to, CollisionTest& out) {
  if(!obj)
    return false;
  return testMove(to,obj->pos,out);
  }

bool DynamicWorld::NpcItem::testMove(const Tempest::Vec3& to, const Tempest::Vec3& pos0, CollisionTest& out) {
  if(!obj)
    return false;
  auto prev = obj->pos;
  auto code = implTryMove(to,pos0,out);
  implSetPosition(prev);
  return code==MoveCode::MC_OK;
  }

DynamicWorld::MoveCode DynamicWorld::NpcItem::tryMove(const Tempest::Vec3& to, CollisionTest& out) {
  if(!obj)
    return MoveCode::MC_Fail;

  resetVirtual();

  auto dp = to - obj->pos;
  if(isNpcMoveSkipped(dp)) {
    // skip-move
    return MoveCode::MC_Skip;
    }

  auto code = implTryMove(to,obj->pos,out);
  switch(code) {
    case MoveCode::MC_Fail:
    case MoveCode::MC_Skip:
      return code;
    case MoveCode::MC_Partial:
      setPosition(out.partial);
      return MoveCode::MC_Partial;
    case MoveCode::MC_OK:
      owner->npcList->onMove(*obj);
      owner->bulletList->onMoveNpc(*obj,*owner->npcList);
      return MoveCode::MC_OK;
    }
  return code;
  }

void DynamicWorld::NpcItem::resetVirtual() {
  if(obj!=nullptr)
    obj->virtualCharacter.reset();
  }

bool DynamicWorld::NpcItem::hasFloorBelow(const Tempest::Vec3& feet) const {
  return obj!=nullptr && owner->landRay(feet+Tempest::Vec3(0,obj->groundOffset()+1.f,0)).hasCol;
  }

void DynamicWorld::NpcItem::setVirtualDiagnosticsEnabled(bool enabled) {
  if(obj==nullptr)
    return;
  obj->virtualDiagnosticsEnabled = enabled;
  if(enabled && obj->virtualDiagnosticMove==nullptr)
    obj->virtualDiagnosticMove = std::make_unique<CharacterVirtualDiagnostics::NpcMove>();
  if(!enabled)
    obj->virtualDiagnosticMove.reset();
  if(obj->virtualCharacter!=nullptr)
    obj->virtualCharacter->setDiagnosticsEnabled(enabled);
  }

bool DynamicWorld::NpcItem::virtualDiagnostics(CharacterVirtualDiagnostics& out) const {
  if(obj==nullptr || obj->virtualCharacter==nullptr || !obj->virtualCharacter->diagnostics(out))
    return false;
  if(obj->virtualDiagnosticMove!=nullptr)
    out.npcMove = *obj->virtualDiagnosticMove;
  return true;
  }

DynamicWorld::NpcItem::VirtualMoveResult DynamicWorld::NpcItem::moveVirtual(
    const Tempest::Vec3& displacement, uint64_t dt, float stepHeight, float maxSlope, float mass, Npc* culprit,
    bool limitStairAdvance, bool bounceOnSteepContact, bool standing) {
  return implMoveVirtual(displacement,dt,stepHeight,maxSlope,mass,culprit,false,limitStairAdvance,bounceOnSteepContact,
                         standing);
  }

DynamicWorld::NpcItem::VirtualMoveResult DynamicWorld::NpcItem::carryVirtual(uint64_t dt, Npc* culprit) {
  return implMoveVirtual({},dt,0,0,0,culprit,true);
  }

DynamicWorld::NpcItem::VirtualMoveResult DynamicWorld::NpcItem::implMoveVirtual(
    const Tempest::Vec3& displacement, uint64_t dt, float stepHeight, float maxSlope, float mass, Npc* culprit, bool carry,
    bool limitStairAdvance, bool bounceOnSteepContact, bool standing) {
  VirtualMoveResult result;
  if(obj==nullptr)
    return result;
  result.position = obj->pos;
  if(!obj->enable || (carry && obj->virtualCharacter==nullptr))
    return result;
  if(obj->virtualCharacter==nullptr) {
    obj->virtualCharacter = owner->world->createVirtualCharacter(obj->radius,obj->h,obj->pos);
    obj->virtualCharacter->setDiagnosticsEnabled(obj->virtualDiagnosticsEnabled);
    }

  const auto initial = obj->pos;
  auto* npcDiagnostic = carry ? nullptr : obj->virtualDiagnosticMove.get();
  if(npcDiagnostic!=nullptr) {
    const auto sequence = npcDiagnostic->sequence+1;
    *npcDiagnostic = {};
    npcDiagnostic->sequence = sequence;
    npcDiagnostic->dt = dt;
    npcDiagnostic->initial = initial;
    npcDiagnostic->requested = displacement;
    }
  // Every character retains its legacy ellipsoid. Movement is sequential, so
  // each sweep sees the latest position of the other characters, including the
  // hero, without applying impulses or moving a neighbour through world walls.
  auto clipNpc = [&](const Tempest::Vec3& target, bool& clipped,
                     CharacterVirtualDiagnostics::NpcCollision* diagnosticCollision) {
    struct Obstacle {
      NpcBody* body = nullptr;
      float depth = 0;
      };
    std::vector<Obstacle> obstacles;
    for(const auto* records:{&owner->npcList->body,&owner->npcList->frozen}) {
      // A script teleport can change a frozen body's X before the next physics
      // tick re-sorts that list, so use current poses for this broad phase.
      for(const auto& record:*records) {
        auto* other = record.body;
        if(other==nullptr || other==obj || !other->enable)
          continue;
        const float reachXZ = obj->maxRXZ+other->maxRXZ;
        const float reachY = obj->maxRY+other->maxRY;
        if(other->pos.x<std::min(initial.x,target.x)-reachXZ || other->pos.x>std::max(initial.x,target.x)+reachXZ ||
           other->pos.z<std::min(initial.z,target.z)-reachXZ || other->pos.z>std::max(initial.z,target.z)+reachXZ ||
           other->pos.y<std::min(initial.y,target.y)-reachY || other->pos.y>std::max(initial.y,target.y)+reachY)
          continue;
        Tempest::Vec3 normal;
        float depth = 0;
        owner->npcList->hasCollision(*obj,*other,normal,depth);
        obstacles.push_back({other,depth});
        }
      }
    const auto delta = target-initial;
    const int count = std::max(1,int(std::ceil(delta.length()/std::max(obj->stepSz*0.5f,1.f))));
    auto accepted = initial;
    for(int i=1;i<=count;++i) {
      obj->setPosition(initial+delta*(float(i)/float(count)));
      for(auto& obstacle:obstacles) {
        Tempest::Vec3 normal;
        float depth = 0;
        owner->npcList->hasCollision(*obj,*obstacle.body,normal,depth);
        if(depth>obstacle.depth+0.01f) {
          result.collision.npc = obstacle.body->toNpc();
          result.collision.npcCol = depth;
          result.collision.normal = Tempest::Vec3::normalize(normal);
          if(diagnosticCollision!=nullptr) {
            diagnosticCollision->npc = obstacle.body->toNpc();
            diagnosticCollision->position = obstacle.body->pos;
            diagnosticCollision->normal = result.collision.normal;
            diagnosticCollision->testedPosition = obj->pos;
            diagnosticCollision->allowedDepth = obstacle.depth;
            diagnosticCollision->rejectedDepth = depth;
            }
          clipped = true;
          break;
          }
        // An overlap after a teleport may separate, but it cannot transfer to
        // another neighbour or grow again after the characters have separated.
        obstacle.depth = std::min(obstacle.depth,depth);
        }
      if(clipped)
        break;
      accepted = obj->pos;
      }
    obj->setPosition(initial);
    return accepted;
    };
  bool requestClipped = false;
  const auto requested = clipNpc(initial+displacement,requestClipped,
                                 npcDiagnostic!=nullptr ? &npcDiagnostic->requestCollision : nullptr)-initial;
  auto moved = carry ? obj->virtualCharacter->carry(dt) :
                       obj->virtualCharacter->move(requested,dt,stepHeight,maxSlope,mass,culprit,limitStairAdvance,standing);
  if(carry && !moved.supportMoved)
    return result;
  bool resolvedClipped = false;
  clipNpc(moved.position,resolvedClipped,npcDiagnostic!=nullptr ? &npcDiagnostic->resolvedCollision : nullptr);
  // Jolt may have slid around a wall or stepped up: an interpolated point on
  // the chord to that endpoint is not necessarily clear of world geometry.
  // If that resolved motion hits an NPC, retain the known-safe starting feet.
  // A capsule can still touch the edge while its feet have already crossed
  // beyond all world collision. Keep the legacy fall guard's vertical floor
  // query: a lower landing is allowed, a bottomless step is not. Test the
  // resolved endpoint too, since wall sliding can change the requested X/Z.
  const auto candidate = resolvedClipped ? initial : moved.position;
  const bool preFall = !hasFloorBelow({candidate.x,std::max(initial.y,candidate.y),candidate.z});
  const auto accepted = preFall ? initial : candidate;
  if(preFall) {
    result.collision.preFall = true;
    result.collision.normal = displacement;
    }
  if(npcDiagnostic!=nullptr) {
    npcDiagnostic->clippedRequest = requested;
    npcDiagnostic->nativePosition = moved.position;
    npcDiagnostic->accepted = accepted;
    npcDiagnostic->requestClipped = requestClipped;
    npcDiagnostic->resolvedClipped = resolvedClipped;
    }
  if(resolvedClipped || preFall) {
    obj->virtualCharacter->setPosition(accepted);
    if(!carry) {
      const auto support = obj->virtualCharacter->move({},0,stepHeight,maxSlope,mass,culprit);
      moved.groundNormal = support.groundNormal;
      moved.groundVelocity = support.groundVelocity;
      moved.grounded = support.grounded;
      moved.steep = support.steep;
      }
    }
  obj->setPosition(accepted);
  owner->npcList->onMove(*obj);
  owner->bulletList->onMoveNpc(*obj,*owner->npcList);
  result.position = accepted;
  result.velocity = moved.velocity;
  const auto blocked = moved.position-accepted;
  if(blocked.quadLength()>0.0001f) {
    const auto normal = Tempest::Vec3::normalize(blocked);
    const float into = Tempest::Vec3::dotProduct(result.velocity,normal);
    if(into>0.f)
      result.velocity -= normal*into;
    }
  result.groundNormal = moved.groundNormal;
  result.groundVelocity = moved.groundVelocity;
  result.grounded = moved.grounded;
  result.steep = moved.steep;
  result.steepContact = moved.steepContact;
  result.steepContactNormal = moved.steepContactNormal;
  result.blocked = preFall;
  result.supportMoved = moved.supportMoved;
  const Tempest::Vec3 horizontal(displacement.x,0,displacement.z);
  const float desiredLength = horizontal.length();
  if(!isNpcMoveSkipped(displacement) && desiredLength>0.1f) {
    const float advancement = Tempest::Vec3::dotProduct(accepted-initial,horizontal)/desiredLength;
    result.advancementRatio = advancement/desiredLength;
    result.blocked |= advancement<=std::max(0.1f,desiredLength*0.1f);
    // Keep neighbour clipping and charge callbacks on their existing path.
    // World sliding can retain half the requested advance, so the old ten
    // percent stall test alone never asks a monster to use its native bounce.
    const bool steepBlocked = bounceOnSteepContact && moved.grounded && moved.steepContact &&
                              !requestClipped && !resolvedClipped && !preFall && result.advancementRatio<.6f;
    result.blocked |= steepBlocked;
    if(result.blocked && !requestClipped && !resolvedClipped && !preFall) {
      result.collision.landCol = true;
      result.collision.normal = steepBlocked ? moved.steepContactNormal :
                                              Tempest::Vec3::normalize(accepted-initial-horizontal);
      }
    }
  if(npcDiagnostic!=nullptr) {
    npcDiagnostic->blocked = result.blocked;
    npcDiagnostic->steepContact = result.steepContact;
    npcDiagnostic->steepContactNormal = result.steepContactNormal;
    npcDiagnostic->advancementRatio = result.advancementRatio;
    }
  return result;
  }

DynamicWorld::MoveCode DynamicWorld::NpcItem::implTryMove(const Tempest::Vec3& to, const Tempest::Vec3& pos0, CollisionTest& out) {
  const auto initial = pos0; // avoid self-reference

  auto r       = obj->stepSz;
  int  count   = 1;
  auto dp      = to-initial;

  if((dp.x*dp.x+dp.z*dp.z)>r*r || dp.y>obj->h*0.5f) {
    const int countXZ = int(std::ceil(std::sqrt(dp.x*dp.x+dp.z*dp.z)/r));
    const int countY  = int(std::ceil(std::abs(dp.y)/(obj->h*0.5f)));

    count = std::max(countXZ,countY);
    }

  float skipNpc    = 0;
  bool  skipLnd    = false;
  bool  secondPass = false;
  for(int i=1; i<=count; ++i) {
    const auto pos = initial + dp*(float(i)/float(count));
    implSetPosition(pos);

    if(!owner->hasCollision(*this,out))
      continue;

    if((skipNpc>=out.npcCol) && (!out.landCol || skipLnd))
      continue;

    if(i>1) {
      // moved a bit
      out.partial = initial + dp*(float(i-1)/float(count));
      implSetPosition(out.partial);
      return MoveCode::MC_Partial;
      }

    implSetPosition(initial);
    if(i==1 && !secondPass) {
      // maybe we were stuck into something(npc) from the start?
      CollisionTest tmpOut = {};
      if(!owner->hasCollision(*this,tmpOut)) {
        return MoveCode::MC_Fail;
        }
      skipNpc    = tmpOut.npcCol;
      skipLnd    = tmpOut.landCol;
      secondPass = true;
      i          = 0;
      continue;
      }

    return MoveCode::MC_Fail;
    }
  return MoveCode::MC_OK;
  }

bool DynamicWorld::NpcItem::hasCollision() const {
  if(!obj)
    return false;
  CollisionTest info;
  return owner->hasCollision(*this,info);
  }
