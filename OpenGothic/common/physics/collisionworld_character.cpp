#include "collisionworld_internal.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>

namespace {
  // Use simulated hulls, including small props. The movable query meshes are
  // duplicate sensor geometry used by the legacy controller and aiming only.
  constexpr uint32_t characterMask = (1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|
                                     (1u<<DynamicWorld::C_Item)|(1u<<DynamicWorld::C_MovableVob)|
                                     (1u<<DynamicWorld::C_Ragdoll)|(1u<<DynamicWorld::C_Fragment);

  // Debris blocks a character from the size that also stops a blade: smaller pieces are
  // C_FragmentSmall, outside characterMask. A body that appeared around this character is
  // passed until they separate.
  class CharacterBodyFilter final : public JPH::BodyFilter {
    public:
      explicit CharacterBodyFilter(const std::vector<JPH::BodyID>& passed):passed(passed) {}
      bool ShouldCollide(const JPH::BodyID& id) const override {
        return std::find(passed.begin(),passed.end(),id)==passed.end();
        }
    private:
      const std::vector<JPH::BodyID>& passed;
    };
  }

struct CollisionWorld::VirtualCharacter::Impl final : JPH::CharacterContactListener {
  explicit Impl(CollisionWorld& world):world(world) {}

  static bool isPushable(const JPH::Body& body) {
    const auto layer = body.GetObjectLayer();
    return body.IsDynamic() && (layer==DynamicWorld::C_Item || layer==DynamicWorld::C_MovableVob ||
                                layer==DynamicWorld::C_Fragment);
    }

  // The character's capsule against a body, within `margin` metres.
  bool overlaps(const JPH::Body& body, float margin) const {
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance=margin;
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
    JPH::CollisionDispatch::sCollideShapeVsShape(character->GetShape(),body.GetShape(),JPH::Vec3::sOne(),JPH::Vec3::sOne(),
                                                 JPH::Mat44(character->GetCenterOfMassTransform()),
                                                 JPH::Mat44(body.GetCenterOfMassTransform()),{},{},settings,hit);
    return hit.HadHit();
    }

  // A controller created inside debris (the first move after a load, a teleport or a reset)
  // passes it as one a new body of debris appeared around, until they separate.
  void passDebrisInside() {
    const QueryBroadPhaseFilter broad(1u<<DynamicWorld::C_Fragment);
    const QueryLayerFilter layers(1u<<DynamicWorld::C_Fragment);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    world.impl->system.GetNarrowPhaseQuery().CollideShape(character->GetShape(),JPH::Vec3::sOne(),character->GetCenterOfMassTransform(),
                                                          settings,JPH::RVec3::sZero(),hits,broad,layers);
    for(const auto& hit:hits.mHits)
      if(std::find(passed.begin(),passed.end(),hit.mBodyID2)==passed.end())
        passed.push_back(hit.mBodyID2);
    }

  // The character stands on a sleeping piece of debris.
  bool sleepingDebrisSupport() const {
    const auto id = character->GetGroundBodyID();
    if(id.IsInvalid())
      return false;
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),id);
    return lock.Succeeded() && lock.GetBody().GetObjectLayer()==DynamicWorld::C_Fragment && !lock.GetBody().IsActive();
    }

  // A passed body is let go once it no longer touches the capsule within its padding.
  void releasePassed() {
    for(size_t i=0;i<passed.size();) {
      JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),passed[i]);
      if(lock.Succeeded() && overlaps(lock.GetBody(),character->GetCharacterPadding()))
        ++i;
      else
        passed.erase(passed.begin()+ptrdiff_t(i));
      }
    }

  static bool blocksAutomaticStairs(const JPH::Body& body) {
    if(isPushable(body))
      return true;
    // C_Object bodies are CollisionBody wrappers, unlike native ragdoll parts.
    // Interactive furniture/containers are not architectural treads. Doors
    // retain their blocking/perception path and ladders use explicit climbing.
    if(body.GetObjectLayer()!=DynamicWorld::C_Object)
      return false;
    const auto* source = reinterpret_cast<const CollisionBody*>(body.GetUserData());
    return source!=nullptr && source->ptrType()==DynamicWorld::P_Interactive;
    }

  bool rejectsSupport(const JPH::Body& body, const JPH::CharacterContact& contact) const {
    if(!isPushable(body) || contact.mIsSensorB)
      return false;
    const auto up = character->GetUp();
    const float mass = 1.f/body.GetMotionProperties()->GetInverseMass();
    if(carrying && body.GetID()==ground && mass>=10.f)
      return false; // This top was accepted before the supporting body advanced.
    const float contactHeight = JPH::Vec3(contact.mPosition-character->GetPosition()).Dot(up);
    return mass<10.f ? contact.mSurfaceNormal.Dot(up)>1.e-3f :
                      contact.mSurfaceNormal.Dot(up)>.99f && contactHeight>.02f;
    }

  JPH::Vec3 finiteGroundNormal(const char* phase) {
    const auto normal=character->GetGroundNormal();
    const float x=normal.GetX(),y=normal.GetY(),z=normal.GetZ();
    const bool finite=std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    const double squared=double(x)*x+double(y)*y+double(z)*z;
    if(finite && squared>1.e-12) {
      lastGroundNormal=normal;
      return normal; // Keep every valid native normal bit-for-bit, without normalization.
    }
    ++groundNormalFallbacks;
    // A zero normal while airborne is Jolt's ordinary no-contact sentinel. Log
    // anomalous support only; all fallback reads still count in diagnostics.
    if(!finite || character->IsSupported()) {
      const auto position=toCentimeters(JPH::Vec3(character->GetPosition()));
      std::fprintf(stderr,"CV_GROUND_NORMAL_FALLBACK phase=%s count=%llu tick_ms=%llu body=%u subshape=%u state=%u "
                          "position_cm=(%.9g,%.9g,%.9g) raw=(%.9g,%.9g,%.9g) fallback=(%.9g,%.9g,%.9g) contacts=%zu\n",
                   phase,static_cast<unsigned long long>(groundNormalFallbacks),
                   static_cast<unsigned long long>(culprit!=nullptr?culprit->world().tickCount():0),
                   character->GetGroundBodyID().GetIndexAndSequenceNumber(),character->GetGroundSubShapeID().GetValue(),
                   unsigned(character->GetGroundState()),double(position.x),double(position.y),double(position.z),
                   double(x),double(y),double(z),double(lastGroundNormal.GetX()),double(lastGroundNormal.GetY()),
                   double(lastGroundNormal.GetZ()),static_cast<size_t>(character->GetActiveContacts().size()));
    }
    return lastGroundNormal;
    }

  JPH::Vec3 initialIdleGravity(JPH::Vec3Arg gravity) {
    // A tangent pose can have tiny negative distances from floating-point
    // contact calculations. Anything beyond one percent of the solver's
    // collision tolerance still needs the ordinary initial recovery solve.
    const float numericalPenetration = .01f*character->GetCharacterVirtualSettings().mCollisionTolerance;
    for(const auto& contact:character->GetActiveContacts())
      if(!contact.mWasDiscarded && !contact.mIsSensorB && contact.mDistance < -numericalPenetration)
        return gravity;

    const auto groundNormal=finiteGroundNormal("initial_idle");
    const auto id = character->GetGroundBodyID();
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),id);
    if(!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsStatic())
      return gravity;
    const auto& body = lock.GetBody();
    const auto subShape = character->GetGroundSubShapeID();
    JPH::SubShapeID remainder;
    const auto* leaf = body.GetShape()->GetLeafShape(subShape,remainder);
    if(leaf==nullptr || (leaf->GetSubType()!=JPH::EShapeSubType::Mesh && leaf->GetSubType()!=JPH::EShapeSubType::Box))
      return gravity;
    // Keep the full subshape ID here: compound and decorated shapes must
    // resolve their leaf's face in the body's world transform.
    const auto face = body.GetWorldSpaceSurfaceNormal(subShape,character->GetGroundPosition());
    constexpr float cosTwoDegrees = .999390827f;
    if(character->IsSlopeTooSteep(face) || face.Dot(groundNormal)<cosTwoDegrees)
      return gravity;
    for(const auto& contact:character->GetActiveContacts()) {
      if(contact.mBodyB!=id || contact.mSubShapeIDB!=subShape || !contact.mHadCollision ||
         contact.mWasDiscarded || contact.mIsSensorB)
        continue;
      // A rounded toe on a tread edge is not an already acquired plane.
      if(face.Dot(contact.mContactNormal)>=cosTwoDegrees && face.Dot(contact.mSurfaceNormal)>=cosTwoDegrees)
        return face*gravity.Dot(face);
      }
    return gravity;
    }

  CharacterVirtualDiagnostics::Body diagnosticBody(JPH::BodyID id, JPH::RVec3Arg point) const {
    CharacterVirtualDiagnostics::Body result;
    result.id = id.GetIndexAndSequenceNumber();
    if(id.IsInvalid())
      return result;
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),id);
    if(!lock.SucceededAndIsInBroadPhase())
      return result;
    const auto& body = lock.GetBody();
    result.present = true;
    result.layer = body.GetObjectLayer();
    result.motionType = int(body.GetMotionType());
    result.nativeUserData = uintptr_t(body.GetUserData());
    result.mass = body.IsDynamic() ? 1.f/body.GetMotionProperties()->GetInverseMass() : 0.f;
    result.position = toCentimeters(JPH::Vec3(body.GetPosition()));
    result.linearVelocity = toCentimeters(body.GetLinearVelocity());
    const auto angular = body.GetAngularVelocity();
    result.angularVelocity = Tempest::Vec3(angular.GetX(),angular.GetY(),angular.GetZ());
    result.pointVelocity = toCentimeters(body.GetPointVelocity(point));
    // Ragdoll / living-body user data is not owned by CollisionBody. Resolve
    // metadata through this world's actual wrappers rather than casting it.
    const auto sourceIt = std::find_if(world.bodies.begin(),world.bodies.end(),
                                      [id](const auto* candidate) { return candidate->id==id; });
    if(sourceIt!=world.bodies.end()) {
      const auto* source = *sourceIt;
      result.pointerType = source->ptrType();
      result.payload = source->payload();
      result.label = source->label();
      }
    return result;
    }

  CharacterVirtualDiagnostics::Pose diagnosticPose() {
    CharacterVirtualDiagnostics::Pose result;
    result.position = toCentimeters(JPH::Vec3(character->GetPosition()));
    result.velocity = toCentimeters(character->GetLinearVelocity());
    result.groundPosition = toCentimeters(JPH::Vec3(character->GetGroundPosition()));
    const auto normal = finiteGroundNormal("diagnostic");
    result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
    result.groundVelocity = toCentimeters(character->GetGroundVelocity());
    result.ground = diagnosticBody(character->GetGroundBodyID(),character->GetGroundPosition());
    result.groundSubShape = character->GetGroundSubShapeID().GetValue();
    result.groundUserData = uintptr_t(character->GetGroundUserData());
    result.groundState = uint8_t(character->GetGroundState());
    const auto inverse = JPH::RMat44::sInverseRotationTranslation(character->GetRotation(),character->GetPosition());
    result.contacts.reserve(character->GetActiveContacts().size());
    for(const auto& contact:character->GetActiveContacts()) {
      CharacterVirtualDiagnostics::Contact out;
      out.body = diagnosticBody(contact.mBodyB,contact.mPosition);
      out.subShape = contact.mSubShapeIDB.GetValue();
      out.contactUserData = uintptr_t(contact.mUserData);
      out.position = toCentimeters(JPH::Vec3(contact.mPosition));
      out.contactNormal = Tempest::Vec3(contact.mContactNormal.GetX(),contact.mContactNormal.GetY(),contact.mContactNormal.GetZ());
      out.surfaceNormal = Tempest::Vec3(contact.mSurfaceNormal.GetX(),contact.mSurfaceNormal.GetY(),contact.mSurfaceNormal.GetZ());
      out.pointVelocity = toCentimeters(contact.mLinearVelocity);
      out.distance = contact.mDistance*100.f;
      const float supportingDistance = character->GetSupportingVolume().SignedDistance(JPH::Vec3(inverse*contact.mPosition));
      out.supportingVolumeDistance = supportingDistance*100.f;
      out.hadCollision = contact.mHadCollision;
      out.discarded = contact.mWasDiscarded;
      out.sensor = contact.mIsSensorB;
      out.canPushCharacter = contact.mCanPushCharacter;
      out.withinSupportingVolume = supportingDistance<=0.f;
      out.walkableSlope = !character->IsSlopeTooSteep(contact.mSurfaceNormal);
      out.supportAllowedByPolicy = true;
      if(out.body.present) {
        JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
        if(lock.SucceededAndIsInBroadPhase())
          out.supportAllowedByPolicy = !rejectsSupport(lock.GetBody(),contact);
        }
      out.supportEligible = out.hadCollision && !out.discarded && !out.sensor &&
                            out.withinSupportingVolume && out.walkableSlope && out.supportAllowedByPolicy;
      result.contacts.push_back(std::move(out));
      }
    return result;
    }

  bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact) override {
    bool rejectSupport = false;
    {
      JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
      if(!lock.SucceededAndIsInBroadPhase() || !isPushable(lock.GetBody()) || contact.mIsSensorB)
        return true;
      // Loose utensils never become a floor. A heavy body's top may support a
      // character placed on it, but walking into it must not acquire that top
      // as a new step. Keep the side contacts, which still stop and push it.
      rejectSupport = rejectsSupport(lock.GetBody(),contact);
      }
    if(!rejectSupport)
      return true;

    // Rejecting a support in Jolt also removes its solver impulse. Retain the
    // real contact point and horizontal push, within the same per-move budget.
    // The capsule's rounded toe must slide through a mug rather than climb it.
    // Sweeps also validate distant future contacts, so only push a point that
    // is already touching the current capsule (including its padding).
    auto local = JPH::Vec3(contact.mPosition-character->GetPosition());
    auto closest = character->GetUp()*std::clamp(local.Dot(character->GetUp()),radius,height-radius);
    if((local-closest).Length()<=radius+character->GetCharacterPadding()+.001f) {
      auto horizontal = contact.mContactNormal-contact.mContactNormal.Dot(character->GetUp())*character->GetUp();
      if(horizontal.IsNearZero()) {
        const auto velocity = character->GetLinearVelocity();
        horizontal = -(velocity-velocity.Dot(character->GetUp())*character->GetUp());
        }
      if(!horizontal.IsNearZero()) {
        auto pushContact = contact;
        pushContact.mContactNormal = horizontal.Normalized();
        pushContact.mDistance = std::max(contact.mDistance,0.f);
        pushContact.mHadCollision = true;
        JPH::CharacterContactSettings settings;
        onContact(pushContact,settings);
        }
      }
    return false;
    }

  void OnContactSolve(const JPH::CharacterVirtual*, const JPH::BodyID& bodyID, const JPH::SubShapeID&,
                      JPH::RVec3Arg point, JPH::Vec3Arg normal, JPH::Vec3Arg, const JPH::PhysicsMaterial*,
                      JPH::Vec3Arg velocity, JPH::Vec3& newVelocity) override {
    JPH::BodyLockRead lock(world.impl->system.GetBodyLockInterface(),bodyID);
    if(!lock.SucceededAndIsInBroadPhase() || !isPushable(lock.GetBody()) ||
       (carrying && bodyID==ground && !character->IsSlopeTooSteep(normal)))
      return;
    const auto up = character->GetUp();
    if(JPH::Vec3(point-character->GetPosition()).Dot(up)<=.02f)
      return; // Existing top support, including a moving body.
    // The rounded capsule can turn even a vertical furniture side into an
    // upward velocity. Side contacts still block us, but cannot lift the feet.
    const float gained = (newVelocity-velocity).Dot(up);
    if(gained>0.f)
      newVelocity -= gained*up;
    }

  void onContact(const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) {
    // Jolt's native limit applies afresh to each contact / stair probe. Dose
    // the impulse here instead, sharing one force budget across the whole move.
    settings.mCanReceiveImpulses = false;
    JPH::BodyLockWrite lock(world.impl->system.GetBodyLockInterface(),contact.mBodyB);
    if(!lock.SucceededAndIsInBroadPhase())
      return;
    const auto& nativeBody = lock.GetBody();
    const auto layer = nativeBody.GetObjectLayer();
    if((layer!=DynamicWorld::C_Item && layer!=DynamicWorld::C_MovableVob && layer!=DynamicWorld::C_Fragment) ||
       !nativeBody.IsDynamic() || contact.mIsSensorB)
      return;
    const float inverseMass = nativeBody.GetMotionProperties()->GetInverseMass();
    if(inverseMass<=0.f)
      return;
    const float bodyMass = 1.f/inverseMass;
    // A rotating utensil's side-point velocity can enter the combined floor
    // constraint and lift the feet even with a perfectly horizontal normal.
    // Keep it as a blocking, pushable object without letting it move the walker.
    // Heavy supports retain their native velocity and continue to carry feet.
    if(bodyMass<10.f)
      settings.mCanPushCharacter = false;
    auto* body = reinterpret_cast<CollisionBody*>(contact.mUserData);
    if(body==nullptr)
      return;
    // Attribute an approaching contact, including the character's weight on a
    // simulated support, rather than a separating or predictive-only contact.
    const auto relative = character->GetLinearVelocity()-contact.mLinearVelocity;
    const float projectedVelocity = relative.Dot(contact.mContactNormal);
    if(projectedVelocity<-0.01f) {
      body->npcContact.store(true,std::memory_order_relaxed);
      body->setTraceCulprit(culprit);
      }
    if(!contact.mHadCollision || pushSeconds<=0.f || pushRemaining<=0.f)
      return;
    // A sleeping piece of debris stays asleep unless the character walks into it: neither the
    // weight of standing on it nor an overlap left by a piece fallen into the capsule pushes it.
    if(layer==DynamicWorld::C_Fragment && !nativeBody.IsActive()) {
      const auto up = character->GetUp();
      const auto across = contact.mContactNormal-contact.mContactNormal.Dot(up)*up;
      if(across.IsNearZero(1.e-4f) || !((relative-relative.Dot(up)*up).Dot(across.Normalized())<-0.01f))
        return;
      }

    // Use the native effective contact mass, including the torque caused by an
    // off-centre push. Penetration recovery has the same damping as Jolt.
    const float deltaVelocity = -projectedVelocity*0.9f-std::min(contact.mDistance,0.f)*0.4f/pushSeconds;
    if(deltaVelocity<=0.f)
      return;
    const auto jacobian = JPH::Vec3(contact.mPosition-nativeBody.GetCenterOfMassPosition()).Cross(contact.mContactNormal);
    const float inverseEffectiveMass = nativeBody.GetInverseInertia().Multiply3x3(jacobian).Dot(jacobian)+inverseMass;
    auto impulse = -(deltaVelocity/inverseEffectiveMass)*contact.mContactNormal;
    // Downward weight is applied independently by CharacterVirtual::Update;
    // do not spend the walking push budget on that supporting contact.
    const float down = impulse.Dot(character->GetUp());
    if(down<0.f)
      impulse -= down*character->GetUp();
    const float magnitude = impulse.Length();
    if(magnitude<=0.f)
      return;

    auto used = std::find_if(pushBodies.begin(),pushBodies.end(),[&](const auto& entry) {return entry.first==contact.mBodyB;});
    if(used==pushBodies.end()) {
      pushBodies.emplace_back(contact.mBodyB,0.f);
      used = std::prev(pushBodies.end());
      }
    // Ordinary walking has less effective effort against a heavier load than
    // a braced push. Keep this continuous in both masses; no furniture classes.
    const float force = character->GetMaxStrength()*pushMass/(pushMass+bodyMass);
    const float available = std::min(pushRemaining,std::max(0.f,force*pushSeconds-used->second));
    const float amount = std::min(magnitude,available);
    if(amount<=0.f)
      return;
    pushRemaining -= amount;
    used->second += amount;
    // The write lock is already held. AddImpulse activates a sleeping body and
    // applies the torque at the real contact point, in native SI units.
    world.impl->system.GetBodyInterfaceNoLock().AddImpulse(contact.mBodyB,impulse*(amount/magnitude),contact.mPosition);
    }

  void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact,
                      JPH::CharacterContactSettings& settings) override { onContact(contact,settings); }
  void OnContactPersisted(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact,
                          JPH::CharacterContactSettings& settings) override { onContact(contact,settings); }

  void OnAdjustBodyVelocity(const JPH::CharacterVirtual*, const JPH::Body& body, JPH::Vec3& linear,
                            JPH::Vec3& angular) override {
    // Carry sweeps through the already integrated world. Predicting the bodies
    // for another interval here would count their motion a second time.
    // An idle update also already accounts for its tracked support's observed
    // motion; its endpoint velocity must not advance that support again.
    if(carrying || (idleSupportMotionObserved && body.GetID()==ground))
      linear = angular = JPH::Vec3::sZero();
    }

  CollisionWorld& world;
  std::unique_ptr<JPH::CharacterVirtual> character;
  std::unique_ptr<CharacterVirtualDiagnostics> diagnostic;
  Npc* culprit = nullptr;
  float pushSeconds = 0.f;
  float pushRemaining = 0.f;
  float pushMass = 70.f; // the character's mass for pushes, kept while the weight is withheld
  std::vector<std::pair<JPH::BodyID,float>> pushBodies;
  std::vector<JPH::BodyID> passed; // debris that appeared around this character
  struct LimitedSteepFace {
    JPH::BodyID body;
    JPH::SubShapeID subShape;
    bool seen = true;
    };
  std::vector<LimitedSteepFace> limitedSteepFaces;
  bool contactsDirty = true;
  bool needsInitialUpdate = true;
  bool carrying = false;
  bool idleSupportMotionObserved = false;
  float radius = 0.f;
  float height = 0.f;
  JPH::Vec3 lastGroundNormal = JPH::Vec3::sAxisY();
  uint64_t groundNormalFallbacks = 0;
  JPH::BodyID ground;
  JPH::RMat44 groundTransform = JPH::RMat44::sIdentity();
  };

CollisionWorld::VirtualCharacter::VirtualCharacter(CollisionWorld& owner, float radius, float height,
                                                  const Tempest::Vec3& feet)
  :impl(std::make_unique<Impl>(owner)) {
  const float r = std::max(toMeters(radius),0.01f);
  const float h = std::max(toMeters(height),2.f*r);
  impl->radius = r;
  impl->height = h;
  JPH::CharacterVirtualSettings settings;
  settings.mShape = new JPH::CapsuleShape(std::max(h*0.5f-r,0.001f),r);
  settings.mShapeOffset = JPH::Vec3(0,h*0.5f,0);
  settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(),-r);
  settings.mCharacterPadding = 0.01f;
  settings.mPredictiveContactDistance = 0.05f;
  settings.mCollisionTolerance = 0.001f;
  settings.mEnhancedInternalEdgeRemoval = true;
  impl->character = std::make_unique<JPH::CharacterVirtual>(&settings,JPH::RVec3(toMeters(feet)),
                                                          JPH::Quat::sIdentity(),&owner.impl->system);
  impl->character->SetListener(impl.get());
  owner.impl->virtualCharacters.push_back(this);
  impl->passDebrisInside();
  }

CollisionWorld::VirtualCharacter::~VirtualCharacter() {
  auto& characters = impl->world.impl->virtualCharacters;
  characters.erase(std::remove(characters.begin(),characters.end(),this),characters.end());
  }

void CollisionWorld::passCharacterOverlaps(const CollisionBody& body) {
  if(impl->virtualCharacters.empty())
    return;
  JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(),body.id);
  if(!lock.Succeeded())
    return;
  for(auto* character:impl->virtualCharacters)
    if(character->impl->overlaps(lock.GetBody(),0.f))
      character->impl->passed.push_back(body.id);
  }

std::unique_ptr<CollisionWorld::VirtualCharacter> CollisionWorld::createVirtualCharacter(
    float radius, float height, const Tempest::Vec3& feet) {
  return std::unique_ptr<VirtualCharacter>(new VirtualCharacter(*this,radius,height,feet));
  }

void CollisionWorld::VirtualCharacter::setPosition(const Tempest::Vec3& feet) {
  impl->character->SetPosition(JPH::RVec3(toMeters(feet)));
  impl->contactsDirty = true;
  impl->needsInitialUpdate = true;
  impl->ground = JPH::BodyID();
  impl->limitedSteepFaces.clear();
  }

void CollisionWorld::VirtualCharacter::setDiagnosticsEnabled(bool enabled) {
  if(enabled && impl->diagnostic==nullptr)
    impl->diagnostic = std::make_unique<CharacterVirtualDiagnostics>();
  if(!enabled)
    impl->diagnostic.reset();
  }

bool CollisionWorld::VirtualCharacter::diagnostics(CharacterVirtualDiagnostics& out) const {
  if(impl->diagnostic==nullptr || !impl->diagnostic->valid)
    return false;
  out = *impl->diagnostic;
  out.groundNormalFallbacks = impl->groundNormalFallbacks;
  return true;
  }

CollisionWorld::CharacterMoveResult CollisionWorld::VirtualCharacter::move(
    const Tempest::Vec3& displacement, uint64_t dt, float stepHeight, float maxSlope, float mass, Npc* culprit,
    bool limitStairAdvance, bool standing) {
  const auto started = std::chrono::steady_clock::now();
  auto& state = *impl;
  auto& character = *state.character;
  auto& world = *state.world.impl;
  const QueryBroadPhaseFilter broadFilter(characterMask);
  const QueryLayerFilter layerFilter(characterMask);
  const CharacterBodyFilter bodyFilter(state.passed);
  const JPH::ShapeFilter shapeFilter;
  state.culprit = culprit;
  if(!state.passed.empty())
    state.releasePassed();
  character.SetMass(std::max(mass,1.f));
  // Jolt uses metres, kilograms and seconds: this is a 450 N human push,
  // capped independently of the character's weight on a supporting body.
  character.SetMaxStrength(std::min(character.GetMass()*DynamicWorld::gravityMS,450.f));
  state.pushMass = character.GetMass();
  const float seconds = float(dt)*0.001f;
  // Zero-dt contact refreshes do not end a physical contact's classification.
  if(dt>0) {
    if(!limitStairAdvance)
      state.limitedSteepFaces.clear();
    else
      for(auto& face:state.limitedSteepFaces)
        face.seen = false;
    }
  state.pushSeconds = seconds;
  state.pushRemaining = character.GetMaxStrength()*seconds;
  state.pushBodies.clear();
  character.SetMaxSlopeAngle(maxSlope);
  if(state.contactsDirty) {
    character.RefreshContacts(broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
    state.contactsDirty = false;
    }
  // A zero-dt refresh after NPC clipping must not replace the main move's trace.
  auto* diagnostic = state.diagnostic!=nullptr && (dt>0 || !state.diagnostic->valid) ? state.diagnostic.get() : nullptr;
  if(diagnostic!=nullptr) {
    diagnostic->valid = false;
    diagnostic->dt = dt;
    diagnostic->requested = displacement;
    diagnostic->requestedVelocity = {};
    diagnostic->stairDesiredVelocity = {};
    diagnostic->maxSlopeRadians = maxSlope;
    diagnostic->stairHeight = stepHeight;
    diagnostic->stairDesiredDistance = 0.f;
    diagnostic->stairAchievedDistance = 0.f;
    diagnostic->stairSupported = false;
    diagnostic->stairCanWalk = false;
    diagnostic->stairBlockedByPolicy = false;
    diagnostic->stairProgressBlocked = false;
    diagnostic->stairProbe = {};
    diagnostic->stairAttempted = false;
    diagnostic->stairSucceeded = false;
    diagnostic->stairObstacleId = uint32_t(-1);
    diagnostic->before = state.diagnosticPose();
    }
  auto inheritedGroundVelocity = character.GetGroundVelocity();
  bool inferredGroundVelocity = false;
  bool climbedStep = false;
  if(seconds>0.f) {
    bool onGround = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
    auto carriedVelocity = JPH::Vec3::sZero();
    if(character.IsSupported() && state.ground==character.GetGroundBodyID() && !state.ground.IsInvalid()) {
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
      if(!lock.SucceededAndIsInBroadPhase())
        onGround = false;
      else if(!lock.GetBody().IsStatic()) {
        // Both direct-matrix movers and fixed-step dynamic bodies have already
        // advanced. Carry the observed displacement at the feet, including the
        // finite rotation, rather than extrapolating their endpoint velocity
        // over a render interval that may contain a different number of steps.
        const auto transform = lock.GetBody().GetWorldTransform();
        const auto position = character.GetPosition();
        carriedVelocity = JPH::Vec3(transform*(state.groundTransform.InversedRotationTranslation()*position)-position)/seconds;
        inheritedGroundVelocity = carriedVelocity;
        inferredGroundVelocity = true;
        }
      }
    character.UpdateGroundVelocity();
    auto groundVelocity = character.GetGroundVelocity();
    if(inferredGroundVelocity) {
      groundVelocity = carriedVelocity;
      // The floor has already advanced vertically. Resolve its new height
      // through penetration recovery / floor sticking; adding its upward
      // speed again would detach the feet from a direct-matrix lift.
      groundVelocity.SetY(0.f);
      }
    auto velocity = toMeters(displacement)/seconds;
    if(onGround)
      velocity += groundVelocity;
    auto gravityStep = JPH::Vec3(0,-DynamicWorld::gravityMS*seconds,0);
    // A standing body may still pass its stance's root sway (S_1HRUN,
    // S_BOWRUN): it stands as firmly as one with zero displacement.
    const bool idle = displacement==Tempest::Vec3() || standing;
    if(idle && onGround) {
      // A supported idle character has static friction. Feeding vertical
      // gravity to Jolt's frictionless contact solver adds a downhill tangent
      // every frame, even on a walkable slope. Keep only pressure into the
      // support; moving platforms still carry us through groundVelocity and
      // Update applies the character's weight independently. Steep slopes and
      // requested walking/jumping retain their ordinary gravity response.
      if(state.needsInitialUpdate)
        gravityStep = state.initialIdleGravity(gravityStep);
      else {
        const auto normal = state.finiteGroundNormal("idle_gravity");
        gravityStep = normal*gravityStep.Dot(normal);
        }
      }
    velocity += gravityStep;
    if(diagnostic!=nullptr)
      diagnostic->requestedVelocity = toCentimeters(velocity);
    // ExtendedUpdate cancels steep-slope motion internally, while preserving
    // this original request for the separate world-stair recipe below.
    character.SetLinearVelocity(velocity);
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    const float step = std::max(toMeters(stepHeight),0.f);
    // A steep, slanted riser can be longer than Jolt's fixed 15 cm probe.
    // Search its possible horizontal run, bounded by the permitted step's
    // height and slope, plus the toe. This changes only the support test;
    // accepted forward travel still uses the original per-frame request.
    const float riserRun = maxSlope>0.f ? std::min(step,step/std::tan(maxSlope)) : step;
    settings.mWalkStairsStepForwardTest = std::max(settings.mWalkStairsStepForwardTest,
                                                  riserRun+state.radius+character.GetCharacterPadding());
    // Run Jolt's ordinary update and floor sticking first. Its automatic stair
    // recipe is applied below only for world geometry, after contact solving
    // has identified pushable objects or interactive walking obstacles.
    settings.mWalkStairsStepUp = JPH::Vec3::sZero();
    settings.mStickToFloorStepDown = JPH::Vec3(0,-step,0);
    const auto oldPosition = character.GetPosition();
    const auto desiredVelocity = character.GetLinearVelocity();
    if(diagnostic!=nullptr)
      diagnostic->stairDesiredVelocity = toCentimeters(desiredVelocity);
    state.idleSupportMotionObserved = inferredGroundVelocity && onGround && idle;
    // Jolt hands the character's weight to its support every update, and the impulse wakes a
    // sleeping support: a character standing still on settled debris withholds it.
    const bool restOnDebris = state.sleepingDebrisSupport();
    if(restOnDebris)
      character.SetMass(0.f);
    character.ExtendedUpdate(seconds,JPH::Vec3(0,-DynamicWorld::gravityMS,0),settings,
                             broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
    if(restOnDebris)
      character.SetMass(state.pushMass);
    state.idleSupportMotionObserved = false;
    // RefreshContacts only classifies the initial overlap. Let the first
    // physical solve after creation/teleport recover penetration with native
    // gravity. Only a verified nonpenetrating static plane can start at rest;
    // edges retain the native contact pose used to enter stairs.
    // A zero-dt refresh does not complete this acquisition.
    state.needsInitialUpdate = false;
    if(diagnostic!=nullptr)
      diagnostic->afterExtendedUpdate = state.diagnosticPose();
    bool stairObstacle = false;
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
         contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f)
        continue;
      for(auto& face:state.limitedSteepFaces)
        if(face.body==contact.mBodyB && face.subShape==contact.mSubShapeIDB)
          face.seen = true;
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
      if(lock.SucceededAndIsInBroadPhase() && Impl::blocksAutomaticStairs(lock.GetBody())) {
        stairObstacle = true;
        if(diagnostic!=nullptr)
          diagnostic->stairObstacleId = contact.mBodyB.GetIndexAndSequenceNumber();
        break;
        }
      }
    // The remainder is the WalkStairs part of Jolt's ExtendedUpdate recipe,
    // including its direction and minimum-distance tests for oblique steps.
    const auto up = character.GetUp();
    // Static friction has a horizontal component on a slope; it is a contact
    // force, not a walking request. Keep support motion in the stair intent.
    const auto stairVelocity = desiredVelocity-gravityStep;
    const auto desiredStep = (stairVelocity-stairVelocity.Dot(up)*up)*seconds;
    const float desiredLength = desiredStep.Length();
    if(diagnostic!=nullptr) {
      const auto achieved = JPH::Vec3(character.GetPosition()-oldPosition);
      const float achievedLength = desiredLength>0.f ? std::max(0.f,achieved.Dot(desiredStep/desiredLength)) : 0.f;
      diagnostic->stairDesiredDistance = desiredLength*100.f;
      diagnostic->stairAchievedDistance = achievedLength*100.f;
      diagnostic->stairSupported = character.IsSupported();
      diagnostic->stairCanWalk = character.CanWalkStairs(desiredVelocity);
      diagnostic->stairBlockedByPolicy = stairObstacle;
      diagnostic->stairProgressBlocked = achievedLength+1.e-4f<desiredLength;
      }
    if(step>0.f && !stairObstacle && desiredLength>0.f && character.CanWalkStairs(desiredVelocity)) {
      const auto forward = desiredStep/desiredLength;
      const auto achieved = JPH::Vec3(character.GetPosition()-oldPosition);
      const float achievedLength = std::max(0.f,achieved.Dot(forward));
      if(achievedLength+1.e-4f<desiredLength) {
        auto direction = forward;
        float best = settings.mWalkStairsCosAngleForwardContact;
        bool extendedSteepFace = false;
        for(const auto& contact:character.GetActiveContacts()) {
          if(!contact.mHadCollision || contact.mWasDiscarded ||
             contact.mSurfaceNormal.Dot(desiredVelocity-contact.mLinearVelocity)>=0.f ||
             !character.IsSlopeTooSteep(contact.mSurfaceNormal))
            continue;
          const auto candidate = contact.mSurfaceNormal.Dot(up)*up-contact.mSurfaceNormal;
          const float length = candidate.Length();
          if(length>1.e-6f && candidate.Dot(forward)/length>best) {
            direction = candidate/length;
            best = direction.Dot(forward);
            }
          if(limitStairAdvance && !contact.mIsSensorB) {
            const auto limited = std::find_if(state.limitedSteepFaces.begin(),state.limitedSteepFaces.end(),[&](const auto& face) {
              return face.body==contact.mBodyB && face.subShape==contact.mSubShapeIDB;
              });
            if(limited!=state.limitedSteepFaces.end()) {
              extendedSteepFace = true;
              continue;
              }
            JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
            if(lock.SucceededAndIsInBroadPhase()) {
              JPH::Shape::SupportingFace face;
              lock.GetBody().GetTransformedShape().GetSupportingFace(contact.mSubShapeIDB,contact.mSurfaceNormal,oldPosition,face);
              if(face.size()>=3) {
                float low = std::numeric_limits<float>::infinity(), high = -low;
                for(const auto& vertex:face) {
                  low = std::min(low,vertex.Dot(up));
                  high = std::max(high,vertex.Dot(up));
                  }
                // Face vertices are relative to the physical feet before this
                // update. Buried geometry below that support level cannot add
                // to the height of the obstacle the monster must climb.
                const float exposedHeight = std::max(0.f,high-std::max(low,0.f));
                if(exposedHeight>step+character.GetCharacterPadding()) {
                  extendedSteepFace = true;
                  // Retain this decision until contact ends: approaching the
                  // crest must not turn the same bank into a new low ledge.
                  state.limitedSteepFaces.push_back({contact.mBodyB,contact.mSubShapeIDB,true});
                  }
                }
              }
            }
          }
        // Keep a monster's stair advance within the remaining animation travel
        // instead of imposing Jolt's 2 cm minimum. A face exposed above the feet
        // by more than the guild step is extended terrain, not a permitted riser:
        // ascend it gradually. Ordinary ledges and shapes without polygonal
        // faces retain the full guild step and the native safe landing sweeps.
        const float forwardDistance = desiredLength-achievedLength;
        const auto forwardStep = direction*(limitStairAdvance ? forwardDistance :
                                            std::max(settings.mWalkStairsMinStepForward,forwardDistance));
        const float stairUp = extendedSteepFace ?
                                std::max(0.f,std::min(step,.12f)-std::max(0.f,achieved.Dot(up))) : step;
        JPH::CharacterVirtual::WalkStairsDiagnostics stairProbe;
        const bool stepped = stairUp>0.f && character.WalkStairs(seconds,up*stairUp,forwardStep,direction*settings.mWalkStairsStepForwardTest,
                                                settings.mWalkStairsStepDownExtra,broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator,
                                                diagnostic!=nullptr ? &stairProbe : nullptr);
        // A completed ordinary step is traversable even if a toe still touches
        // its riser. The limited rise against an extended bank is not one.
        climbedStep = stepped && !extendedSteepFace;
        if(diagnostic!=nullptr) {
          diagnostic->stairAttempted = true;
          diagnostic->stairSucceeded = stepped;
          static constexpr const char* reasons[]={"notAttempted","upBlocked","noSteep","noHorizontal","noProgress","noFloor",
                                                   "noForwardTest","testNoProgress","testNoFloor","testSteep","noHeight","success"};
          auto& probe=diagnostic->stairProbe;
          probe.reason = reasons[unsigned(stairProbe.mResult)];
          probe.upPosition = toCentimeters(JPH::Vec3(stairProbe.mUpPosition));
          probe.forwardPosition = toCentimeters(JPH::Vec3(stairProbe.mForwardPosition));
          probe.finalPosition = toCentimeters(JPH::Vec3(stairProbe.mFinalPosition));
          probe.testPosition = toCentimeters(JPH::Vec3(stairProbe.mTestPosition));
          probe.testFinalPosition = toCentimeters(JPH::Vec3(stairProbe.mTestFinalPosition));
          const auto normal=stairProbe.mLandingNormal, testNormal=stairProbe.mTestLandingNormal;
          probe.landingNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
          probe.testLandingNormal = Tempest::Vec3(testNormal.GetX(),testNormal.GetY(),testNormal.GetZ());
          probe.upFraction = stairProbe.mUpFraction;
          probe.landingFraction = stairProbe.mLandingFraction;
          probe.testLandingFraction = stairProbe.mTestLandingFraction;
          }
        }
      }
    }
  if(dt>0)
    state.limitedSteepFaces.erase(std::remove_if(state.limitedSteepFaces.begin(),state.limitedSteepFaces.end(),
                                                [](const auto& face) { return !face.seen; }),state.limitedSteepFaces.end());
  if(diagnostic!=nullptr) {
    if(seconds<=0.f)
      diagnostic->afterExtendedUpdate = state.diagnosticPose();
    diagnostic->afterStairs = state.diagnosticPose();
    diagnostic->valid = true;
    }
  const auto previousGround = state.ground;
  state.ground = character.GetGroundBodyID();
  if(!state.ground.IsInvalid()) {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(lock.Succeeded())
      state.groundTransform = lock.GetBody().GetWorldTransform();
    else
      state.ground = JPH::BodyID();
    }
  CharacterMoveResult result;
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  // Jolt keeps its requested velocity after moving. Resolve the carried
  // velocity against actual contacts so downhill acceleration survives, while
  // stair snaps and penetration recovery never turn into airborne momentum.
  auto solvedVelocity = character.GetLinearVelocity();
  for(int iteration=0;iteration<2;++iteration) {
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB)
        continue;
      const float into = (solvedVelocity-contact.mLinearVelocity).Dot(contact.mContactNormal);
      if(into<0.f)
        solvedVelocity -= contact.mContactNormal*into;
      }
    }
  result.velocity = toCentimeters(solvedVelocity);
  result.groundVelocity = toCentimeters(inferredGroundVelocity && previousGround==state.ground ?
                                        inheritedGroundVelocity : character.GetGroundVelocity());
  const auto normal = state.finiteGroundNormal("move_output");
  result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
  result.grounded = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
  result.steep = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnSteepGround;
  if(!climbedStep) {
    const auto horizontal = toMeters(Tempest::Vec3(displacement.x,0,displacement.z));
    float strongest = -.01f*horizontal.Length();
    for(const auto& contact:character.GetActiveContacts()) {
      if(!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB ||
         !character.IsSlopeTooSteep(contact.mSurfaceNormal))
        continue;
      const float into = contact.mSurfaceNormal.Dot(horizontal);
      if(into>=strongest)
        continue;
      JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),contact.mBodyB);
      if(!lock.SucceededAndIsInBroadPhase() || lock.GetBody().IsDynamic())
        continue;
      strongest = into;
      result.steepContact = true;
      const auto normal = contact.mSurfaceNormal;
      result.steepContactNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
      }
    }
  world.characterMilliseconds += std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

CollisionWorld::CharacterMoveResult CollisionWorld::VirtualCharacter::carry(uint64_t dt) {
  auto& state = *impl;
  auto& character = *state.character;
  auto& world = *state.world.impl;
  CharacterMoveResult result;
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  if(dt==0 || character.GetGroundState()!=JPH::CharacterBase::EGroundState::OnGround ||
     state.ground.IsInvalid() || state.ground!=character.GetGroundBodyID())
    return result;
  JPH::RMat44 transform;
  {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(!lock.SucceededAndIsInBroadPhase() || lock.GetBody().IsStatic())
      return result;
    transform = lock.GetBody().GetWorldTransform();
  }
  const auto position = character.GetPosition();
  const auto delta = JPH::Vec3(transform*(state.groundTransform.InversedRotationTranslation()*position)-position);
  state.groundTransform = transform;
  if(delta.IsNearZero(1.e-7f))
    return result;
  const auto started = std::chrono::steady_clock::now();
  const QueryBroadPhaseFilter broadFilter(characterMask);
  const QueryLayerFilter layerFilter(characterMask);
  const CharacterBodyFilter bodyFilter(state.passed);
  const JPH::ShapeFilter shapeFilter;
  const float seconds = float(dt)*.001f;
  const auto velocity = character.GetLinearVelocity();
  state.pushSeconds = state.pushRemaining = 0.f;
  state.carrying = true;
  // This is a collision sweep of the support's observed motion, never a pose
  // assignment. No second root input, gravity impulse, push budget or stairs.
  character.SetLinearVelocity(delta/seconds);
  character.Update(seconds,JPH::Vec3::sZero(),broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
  // A rising support can separate relative to the frozen body's zero velocity.
  // Re-evaluate support at the swept endpoint without that velocity test.
  character.SetLinearVelocity(JPH::Vec3::sZero());
  character.RefreshContacts(broadFilter,layerFilter,bodyFilter,shapeFilter,world.allocator);
  state.carrying = false;
  character.SetLinearVelocity(velocity);
  character.UpdateGroundVelocity();
  state.ground = character.GetGroundBodyID();
  if(!state.ground.IsInvalid()) {
    JPH::BodyLockRead lock(world.system.GetBodyLockInterface(),state.ground);
    if(lock.SucceededAndIsInBroadPhase())
      state.groundTransform = lock.GetBody().GetWorldTransform();
    else
      state.ground = JPH::BodyID();
  }
  result.position = toCentimeters(JPH::Vec3(character.GetPosition()));
  result.velocity = toCentimeters(velocity);
  result.groundVelocity = toCentimeters(delta/seconds);
  const auto normal = state.finiteGroundNormal("carry_output");
  result.groundNormal = Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ());
  result.grounded = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnGround;
  result.steep = character.GetGroundState()==JPH::CharacterBase::EGroundState::OnSteepGround;
  result.supportMoved = true;
  world.characterMilliseconds += std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
  return result;
  }

bool CollisionWorld::CollisionBody::keepSleepingItem(const Tempest::Vec3& positionCm) const {
  if((owner->buoyancy && inWater) || itemRecovered ||
     (owner->itemWake && itemWakeUntilStep>=owner->impl->simulationStep && itemWakeUntilStep!=0))
    return true;
  const auto position=toMeters(positionCm);
  const float range=toMeters(DynamicWorld::characterItemRange);
  for(const auto* character:owner->impl->virtualCharacters)
    if((position-character->impl->character->GetPosition()).LengthSq()<=range*range)
      return true;
  return false;
  }
