#include "cloth.h"
#include "collisionworld.h"
#include "dynamicworld.h"
#include "frameprofile.h"

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Geometry/RayCapsule.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace {
  Tempest::Vec3 project(const Tempest::Matrix4x4& matrix, Tempest::Vec3 point) {
    matrix.project(point);
    return point;
    }
  bool finite(const Tempest::Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }
  bool segmentTriangle(const Tempest::Vec3& from, const Tempest::Vec3& to,
                       const Tempest::Vec3& a, const Tempest::Vec3& b, const Tempest::Vec3& c) {
    const auto dir=to-from, e1=b-a, e2=c-a;
    const auto p=Tempest::Vec3::crossProduct(dir,e2);
    const float det=Tempest::Vec3::dotProduct(e1,p);
    if(std::abs(det)<1.e-6f) return false;
    const auto t=from-a;
    const float u=Tempest::Vec3::dotProduct(t,p)/det;
    if(u<0.f || u>1.f) return false;
    const auto q=Tempest::Vec3::crossProduct(t,e1);
    const float v=Tempest::Vec3::dotProduct(dir,q)/det;
    if(v<0.f || u+v>1.f) return false;
    const float distance=Tempest::Vec3::dotProduct(e2,q)/det;
    return distance>=0.f && distance<=1.f;
    }
  JPH::Vec3 limited(JPH::Vec3Arg velocity) {
    if(velocity.IsNaN()) return JPH::Vec3::sZero();
    const float speed=velocity.Length();
    if(!std::isfinite(speed)) return JPH::Vec3::sZero();
    return speed>Cloth::maxVelocityMS ? velocity*(Cloth::maxVelocityMS/speed) : velocity;
    }
  JPH::Vec3 closestOnSegment(JPH::Vec3Arg point,JPH::Vec3Arg from,JPH::Vec3Arg to) {
    const auto axis=to-from;
    const float lengthSq=axis.LengthSq();
    return from+axis*(lengthSq>1.e-12f ? std::clamp((point-from).Dot(axis)/lengthSq,0.f,1.f) : 0.f);
    }
  }

struct Cloth::Instance::Impl {
  struct Contact {
    uint32_t vertex = 0;
    JPH::BodyID body;
    Tempest::Vec3 normal;
    };
  Cloth::Impl* owner = nullptr;
  JPH::BodyID body;
  Tempest::Matrix4x4 matrix, inverse;
  Tempest::Vec3 localCenter;
  std::vector<Tempest::Vec3> source, simulated, previous, rendered;
  std::vector<uint32_t> mapping, indices;
  std::vector<JPH::Vec3> rest;
  std::vector<bool> pinned;
  JPH::Ref<JPH::SoftBodySharedSettings> settings;
  ShaderReference shader;
  UpdateCallback update;
  std::string visual;
  uint32_t zenId = uint32_t(-1);
  float stiffness = .8f, weight = 0, targetWeight = 0, shelterTimer = 0;
  uint32_t substeps = 4;
  float linearDamping = 1.5f;
  float maxAnchorError = 0, maxSpeed = 0, maxSpeedEver = 0, transitionMaxDelta = 0;
  std::vector<Contact> contacts;
  size_t contactCount = 0;
  uint32_t maxSpeedVertex = uint32_t(-1), maxSpeedEverVertex = uint32_t(-1);
  Tempest::Vec3 maxSpeedSource, maxSpeedWorld, maxSpeedContactNormal;
  Tempest::Vec3 maxSpeedEverSource, maxSpeedEverWorld, maxSpeedEverContactNormal;
  bool maxSpeedContact = false, maxSpeedEverContact = false;
  JPH::BodyID maxSpeedContactBody, maxSpeedEverContactBody;
  std::vector<WindOccluder> windOccluders;
  uint64_t projectileHits = 0, arrowPasses = 0, recoveryCount = 0;
  uint64_t simulationSteps = 0;
  double accumulated = 0;
  float boundsRadius = 0;
  bool exposed = true, recover = false, visible = true, stepPending = false, dirty = true;
  Tempest::Vec3 supportFrom, supportTo;
  float supportRadiusCm=0;
  std::vector<JPH::Vec3> supportPrevious;

  Tempest::Vec3 center() const { return project(matrix,localCenter); }
  Tempest::Vec3 translation() const { return Tempest::Vec3(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)); }
  Tempest::Vec3 reference(size_t vertex) const {
    if(!shader) return source[vertex];
    auto ret=shader(uint32_t(vertex),source[vertex],project(matrix,source[vertex]));
    return finite(ret) ? ret : source[vertex];
    }
  };

struct Cloth::Impl : JPH::SoftBodyContactListener {
  CollisionWorld& world;
  std::vector<std::weak_ptr<Instance>> instances;
  Tempest::Vec3 observer, wind;
  std::function<bool(const Tempest::Vec3&)> exposure;
  std::function<bool(const Tempest::Vec3&,float)> visibility;
  double frameSeconds = 0;
  bool enabled = true, hasObserver = false;
  float windScale = 1, currentWindScale = 1;
  explicit Impl(CollisionWorld& world):world(world) {}

  auto& system() { return world.physicsSystem(); }
  void OnSoftBodyContactAdded(const JPH::Body& native,const JPH::SoftBodyManifold& manifold) override {
    if(native.GetObjectLayer()!=DynamicWorld::C_Cloth || native.GetUserData()==0) return;
    auto& body=*reinterpret_cast<Instance::Impl*>(native.GetUserData());
    body.contacts.clear();
    const auto& vertices=manifold.GetVertices();
    const auto transform=native.GetCenterOfMassTransform();
    // Jolt resets mHasContact and its plane immediately after this callback.
    // Keep the native identity and normal until the main-thread finishStep.
    for(uint32_t i=0;i<vertices.size();++i) {
      if(!manifold.HasContact(vertices[i])) continue;
      const auto normal=transform.Multiply3x3(vertices[i].mCollisionPlane.GetNormal());
      body.contacts.push_back({i,manifold.GetContactBodyID(vertices[i]),
                                Tempest::Vec3(normal.GetX(),normal.GetY(),normal.GetZ())});
      }
    }
  void remove(Instance::Impl& body) {
    if(body.body.IsInvalid()) return;
    auto& native=system().GetBodyInterface();
    // CustomUpdate bodies are registered for locks/lifetime but never added
    // to broadphase: the world must not simulate them again at 60 Hz.
    if(native.IsAdded(body.body)) native.RemoveBody(body.body);
    native.DestroyBody(body.body);
    body.body=JPH::BodyID();
    body.contactCount=0;
    body.contacts.clear();
    body.accumulated=0;
    }
  void create(Instance::Impl& body) {
    if(!body.body.IsInvalid()) return;
    body.settings=new JPH::SoftBodySharedSettings();
    auto& settings=*body.settings;
    body.rest.clear();
    body.rest.resize(body.pinned.size());
    for(size_t i=0;i<body.source.size();++i) {
      const auto position=CollisionWorld::toMeters(project(body.matrix,body.source[i])-body.translation());
      body.rest[body.mapping[i]]=position;
      }
    std::vector<float> vertexArea(body.rest.size(),0.f);
    std::set<std::array<uint32_t,3>> seen;
    for(size_t i=0;i+2<body.indices.size();i+=3) {
      std::array<uint32_t,3> face={body.mapping[body.indices[i]],body.mapping[body.indices[i+1]],body.mapping[body.indices[i+2]]};
      auto key=face;
      std::sort(key.begin(),key.end());
      if(key[0]==key[1] || key[1]==key[2] || !seen.insert(key).second) continue;
      const float twiceArea=(body.rest[face[1]]-body.rest[face[0]]).Cross(body.rest[face[2]]-body.rest[face[0]]).Length();
      if(twiceArea<1.e-6f) continue;
      settings.mFaces.emplace_back(face[0],face[1],face[2]);
      for(auto vertex:face) vertexArea[vertex]+=twiceArea/6.f;
      }
    if(settings.mFaces.empty()) return;
    // Each triangle contributes one third of its area to its particles. UV
    // seams and reversed back faces were welded above, so subdivision changes
    // resolution rather than the weight of the same piece of cloth.
    constexpr float densityKgPerSquareMeter=.35f;
    settings.mVertices.reserve(body.rest.size());
    for(size_t i=0;i<body.rest.size();++i) {
      JPH::Float3 point;
      body.rest[i].StoreFloat3(&point);
      const float inverseMass=1.f/std::max(vertexArea[i]*densityKgPerSquareMeter,.00025f);
      settings.mVertices.emplace_back(point,JPH::Float3(0,0,0),body.pinned[i] ? 0.f : inverseMass);
      }
    const float compliance=1.e-6f+(1.f-body.stiffness)*2.e-5f;
    const JPH::SoftBodySharedSettings::VertexAttributes attributes(compliance,compliance,compliance*10.f,
        JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance,1.03f);
    settings.CreateConstraints(&attributes,1,JPH::SoftBodySharedSettings::EBendType::Distance);
    settings.Optimize();
    JPH::SoftBodyCreationSettings creation(&settings,CollisionWorld::toMeters(body.translation()),JPH::Quat::sIdentity(),DynamicWorld::C_Cloth);
    creation.mUserData=reinterpret_cast<uint64_t>(&body);
    // Fixed particles and geodesic long-range attachments bound the cloth.
    // Jolt iterations are integration substeps, not repeated solver sweeps.
    // Keep the original 60 Hz material's 1/240 or 1/480 s XPBD step when the
    // outer cloth clock runs at 30 Hz. Otherwise coarse, side-pinned flags
    // acquire different compliance and keep moving long after wind stops.
    float freeSpan=0;
    for(const auto& attachment:settings.mLRAConstraints)
      freeSpan=std::max(freeSpan,attachment.mMaxDistance);
    body.substeps=freeSpan>6.f ? 16 : 8;
    creation.mNumIterations=body.substeps;
    // Together with aerodynamic drag this is close to critical damping for
    // a ship flag's seven-metre free span. Excess damping makes those long
    // side-pinned flags creep towards their hanging pose for many seconds.
    creation.mLinearDamping=body.linearDamping;
    creation.mMaxLinearVelocity=Cloth::maxVelocityMS;
    creation.mVertexRadius=.005f;
    creation.mUpdatePosition=false;
    creation.mAllowSleeping=false;
    creation.mFacesDoubleSided=true;
    auto* native=system().GetBodyInterface().CreateSoftBody(creation);
    if(native==nullptr) return;
    body.body=native->GetID();
    // Keep constraints authored at rest. Seeding positions never changes their
    // rest lengths, and pins always start at their real world attachments.
    auto& motion=*static_cast<JPH::SoftBodyMotionProperties*>(native->GetMotionProperties());
    for(size_t i=0;i<body.source.size();++i) {
      const auto vertex=body.mapping[i];
      if(body.pinned[vertex]) continue;
      auto& particle=motion.GetVertex(vertex);
      particle.mPosition=CollisionWorld::toMeters(project(body.matrix,body.reference(i))-body.translation());
      particle.mPreviousPosition=particle.mPosition;
      }
    for(size_t i=0;i<body.source.size();++i)
      body.simulated[i]=project(body.inverse,body.translation()+CollisionWorld::toCentimeters(motion.GetVertex(body.mapping[i]).mPosition));
    body.previous=body.simulated;
    body.accumulated=0;
    }

  template<class F> void each(F&& callback) {
    for(auto it=instances.begin();it!=instances.end();) {
      if(auto instance=it->lock()) {
        callback(*instance->impl);
        ++it;
        } else it=instances.erase(it);
      }
    }
  };

Cloth::Instance::Instance(std::unique_ptr<Impl> impl):impl(std::move(impl)) {}
Cloth::Instance::~Instance() { if(impl->owner!=nullptr) impl->owner->remove(*impl); }
void Cloth::Instance::setMatrix(const Tempest::Matrix4x4& matrix) {
  if(matrix==impl->matrix) return;
  if(impl->owner!=nullptr) impl->owner->remove(*impl);
  impl->matrix=matrix;
  impl->inverse=matrix;
  impl->inverse.inverse();
  impl->weight=0;
  impl->shelterTimer=0;
  impl->dirty=true;
  impl->boundsRadius=0;
  for(const auto& vertex:impl->source)
    impl->boundsRadius=std::max(impl->boundsRadius,2.f*(project(matrix,vertex)-impl->center()).length());
  }
void Cloth::Instance::setAttachmentCapsule(const Tempest::Vec3& from,const Tempest::Vec3& to,float radius) {
  if(!finite(from) || !finite(to) || !std::isfinite(radius) || radius<=0.f) {
    impl->supportRadiusCm=0;
    return;
    }
  impl->supportFrom=from;
  impl->supportTo=to;
  impl->supportRadiusCm=radius;
  }
void Cloth::Instance::setLinearDamping(float value) {
  if(!std::isfinite(value) || value<0.f) return;
  impl->linearDamping=value;
  if(impl->body.IsInvalid() || impl->owner==nullptr) return;
  JPH::BodyLockWrite lock(impl->owner->system().GetBodyLockInterface(),impl->body);
  if(lock.Succeeded()) lock.GetBody().GetMotionProperties()->SetLinearDamping(value);
  }
void Cloth::Instance::setShaderReference(ShaderReference reference) { impl->shader=std::move(reference); }
void Cloth::Instance::setUpdateCallback(UpdateCallback callback) { impl->update=std::move(callback); }
const std::vector<Tempest::Vec3>& Cloth::Instance::positionsCm() const { return impl->rendered; }
float Cloth::Instance::simulationWeight() const { return impl->weight; }
bool Cloth::Instance::active() const { return !impl->body.IsInvalid(); }

Cloth::Cloth(CollisionWorld& world):impl(std::make_unique<Impl>(world)) {
  world.cloth=this;
  impl->system().SetSoftBodyContactListener(impl.get());
  }
Cloth::~Cloth() {
  impl->each([&](Instance::Impl& body) { impl->remove(body); body.owner=nullptr; });
  impl->system().SetSoftBodyContactListener(nullptr);
  impl->world.cloth=nullptr;
  }
std::shared_ptr<Cloth::Instance> Cloth::add(const std::vector<Tempest::Vec3>& source,
                                         const std::vector<uint32_t>& indices,
                                         const std::vector<uint32_t>& pins,
                                         const Tempest::Matrix4x4& matrix,float stiffness,
                                         std::string visual,uint32_t zenId) {
  if(source.empty() || indices.empty() || pins.empty() || indices.size()%3!=0) return {};
  if(std::any_of(source.begin(),source.end(),[](const auto& v) { return !finite(v); })) return {};
  for(auto index:indices) if(index>=source.size()) return {};
  for(auto index:pins) if(index>=source.size()) return {};
  auto body=std::make_unique<Instance::Impl>();
  body->owner=impl.get();
  body->source=body->simulated=body->previous=body->rendered=source;
  body->indices=indices;
  body->matrix=body->inverse=matrix;
  body->inverse.inverse();
  body->stiffness=std::clamp(stiffness,0.f,1.f);
  body->visual=std::move(visual);
  body->zenId=zenId;
  std::map<std::tuple<int64_t,int64_t,int64_t>,uint32_t> welded;
  auto min=source.front(),max=source.front();
  for(const auto& vertex:source) {
    // 0.01 cm: weld UV seams and the reversed back faces of Gothic cloth.
    const auto key=std::make_tuple(int64_t(std::llround(vertex.x*100)),int64_t(std::llround(vertex.y*100)),int64_t(std::llround(vertex.z*100)));
    const auto result=welded.emplace(key,uint32_t(welded.size()));
    body->mapping.push_back(result.first->second);
    min.x=std::min(min.x,vertex.x); min.y=std::min(min.y,vertex.y); min.z=std::min(min.z,vertex.z);
    max.x=std::max(max.x,vertex.x); max.y=std::max(max.y,vertex.y); max.z=std::max(max.z,vertex.z);
    }
  body->localCenter=(min+max)*.5f;
  // A free corner can fall by its entire span around a fixed attachment.
  // Twice the authored half-diagonal bounds that swing, including long sails.
  for(const auto& vertex:source)
    body->boundsRadius=std::max(body->boundsRadius,2.f*(project(matrix,vertex)-body->center()).length());
  body->pinned.resize(welded.size(),false);
  for(auto index:pins) body->pinned[body->mapping[index]]=true;
  if(std::all_of(body->pinned.begin(),body->pinned.end(),[](bool pin) { return pin; })) return {};
  auto ret=std::shared_ptr<Instance>(new Instance(std::move(body)));
  impl->instances.push_back(ret);
  return ret;
  }
void Cloth::setEnabled(bool value) {
  // World::tick applies the setting every tick: only a real toggle refreshes every view.
  if(impl->enabled==value) return;
  impl->enabled=value;
  impl->each([](Instance::Impl& body) { body.dirty=true; });
  // The static world-cloth fallback switches in this same call stack. Update
  // the individual views now so a render between physics ticks has no gap.
  finishFrame();
  }
bool Cloth::enabled() const { return impl->enabled; }
void Cloth::setObserver(const Tempest::Vec3& position) { if(finite(position)) { impl->observer=position; impl->hasObserver=true; } }
void Cloth::setWind(const Tempest::Vec3& wind) { impl->wind=finite(wind) ? wind : Tempest::Vec3(); }
void Cloth::setWindScale(float value) { if(std::isfinite(value)) impl->windScale=std::clamp(value,0.f,4.f); }
float Cloth::windScale() const { return impl->currentWindScale; }
void Cloth::setExposureTest(std::function<bool(const Tempest::Vec3&)> test) {
  impl->exposure=std::move(test);
  impl->each([](Instance::Impl& body) { body.shelterTimer=0; });
  }
void Cloth::setVisibilityTest(std::function<bool(const Tempest::Vec3&,float)> test) {
  impl->visibility=std::move(test);
  }
size_t Cloth::bodyCount() const {
  size_t count=0;
  for(const auto& weak:impl->instances) if(auto instance=weak.lock()) if(instance->active()) ++count;
  return count;
  }

void Cloth::prepareFrame(uint64_t dt) {
  FrameProfile::Scope timing(FrameProfile::Zone::Cloth);
  const float seconds=std::min(float(dt)*.001f,.1f);
  impl->frameSeconds=std::min(double(dt)*.001,.1);
  impl->currentWindScale+=(impl->windScale-impl->currentWindScale)*(1.f-std::exp(-seconds*1.5f));
  impl->each([&](Instance::Impl& body) {
    if(body.recover) {
      impl->remove(body);
      body.recover=false;
      body.weight=0;
      }
    const auto center=body.center();
    body.visible=!impl->visibility || impl->visibility(center,body.boundsRadius);
    const float distance=impl->hasObserver ? (center-impl->observer).length() : radiusCm+1.f;
    float target=impl->enabled ? std::clamp((radiusCm-distance)/(radiusCm-blendStartCm),0.f,1.f) : 0.f;
    body.targetWeight=target*target*(3.f-2.f*target);
    body.shelterTimer-=seconds;
    if(body.targetWeight>0.f && body.shelterTimer<=0.f) {
      // BSP/portal rooms are authoritative. Upward solid rays cannot tell a
      // ship's own yard, mast or deck from an indoor ceiling in the same mesh.
      body.exposed=!impl->exposure || impl->exposure(center);
      body.windOccluders.clear();
      body.shelterTimer=.5f;
      }
    if(body.targetWeight>0.f && body.body.IsInvalid()) impl->create(body);
    const float oldWeight=body.weight;
    if(body.body.IsInvalid()) body.weight=0;
    else body.weight+=std::clamp(body.targetWeight-body.weight,-seconds*2.f,seconds*2.f);
    const float weightChange=std::abs(oldWeight-body.weight);
    body.dirty|=weightChange>0.f;
    if(weightChange>0.f) {
      for(size_t i=0;i<body.source.size();++i)
        body.transitionMaxDelta=std::max(body.transitionMaxDelta,(body.simulated[i]-body.reference(i)).length()*weightChange);
      }
    if(body.weight<=0.f && body.targetWeight<=0.f) impl->remove(body);
    });
  }
void Cloth::prepareStep(float seconds,uint32_t nativeCollisionSteps) {
  (void)nativeCollisionSteps;
  impl->each([&](Instance::Impl& body) {
    if(body.body.IsInvalid() || !body.stepPending) return;
    body.contacts.clear();
    JPH::BodyLockWrite lock(impl->system().GetBodyLockInterface(),body.body);
    if(!lock.Succeeded()) return;
    auto& motion=*static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    // Cloth has its own fixed 30 Hz clock. Ragdoll/fragment collision steps
    // neither add work here nor change the cloth's XPBD timestep.
    motion.SetNumIterations(body.substeps);
    if(body.supportRadiusCm>0.f) {
      body.supportPrevious.resize(motion.GetVertices().size());
      for(size_t i=0;i<body.supportPrevious.size();++i)
        body.supportPrevious[i]=motion.GetVertex(uint32_t(i)).mPosition;
      }
    const auto wind=body.exposed ? CollisionWorld::toMeters(impl->wind*100.f*impl->currentWindScale) : JPH::Vec3::sZero();
    for(auto& vertex:motion.GetVertices()) {
      if(vertex.mInvMass==0.f) { vertex.mVelocity=JPH::Vec3::sZero(); continue; }
      // Aerodynamic drag gives a bounded, gradual response to gusts. Native
      // damping remains active in calm weather to settle hanging cloth.
      vertex.mVelocity=limited(vertex.mVelocity+(wind-vertex.mVelocity)*(.8f*seconds));
      }
    motion.CustomUpdate(seconds,lock.GetBody(),impl->system());
    ++body.simulationSteps;
    });
  }
void Cloth::finishStep() {
  impl->each([&](Instance::Impl& body) {
    if(body.body.IsInvalid()) { body.maxSpeed=0; return; }
    if(!body.stepPending) return;
    JPH::BodyLockWrite lock(impl->system().GetBodyLockInterface(),body.body);
    if(!lock.Succeeded()) return;
    auto& native=lock.GetBody();
    auto& motion=*static_cast<JPH::SoftBodyMotionProperties*>(native.GetMotionProperties());
    if(body.supportRadiusCm>0.f) {
      const auto from=CollisionWorld::toMeters(project(body.matrix,body.supportFrom)-body.translation());
      const auto to=CollisionWorld::toMeters(project(body.matrix,body.supportTo)-body.translation());
      const auto x=project(body.matrix,{1,0,0})-body.translation();
      const auto y=project(body.matrix,{0,1,0})-body.translation();
      const auto z=project(body.matrix,{0,0,1})-body.translation();
      const float radius=(body.supportRadiusCm+.5f)*.01f*std::max({x.length(),y.length(),z.length()});
      const auto axis=to-from;
      const float halfHeight=axis.Length()*.5f;
      const auto inverseRotation=JPH::Quat::sFromTo(JPH::Vec3::sAxisY(),axis.NormalizedOr(JPH::Vec3::sAxisY())).Conjugated();
      const auto midpoint=(from+to)*.5f;
      for(size_t i=0;i<motion.GetVertices().size();++i) {
        auto& vertex=motion.GetVertex(uint32_t(i));
        if(vertex.mInvMass==0.f) continue;
        const auto previous=i<body.supportPrevious.size() ? body.supportPrevious[i] : vertex.mPreviousPosition;
        const auto sweep=vertex.mPosition-previous;
        const float fraction=JPH::RayCapsule(inverseRotation*(previous-midpoint),inverseRotation*sweep,halfHeight,radius);
        if(fraction>0.f && fraction<1.f) {
          const auto contact=previous+sweep*fraction;
          const auto normal=(contact-closestOnSegment(contact,from,to)).NormalizedOr(JPH::Vec3::sAxisZ());
          const float inward=(vertex.mPosition-contact).Dot(normal);
          if(inward<0.f) vertex.mPosition-=normal*inward;
          const float incoming=vertex.mVelocity.Dot(normal);
          if(incoming<0.f) vertex.mVelocity-=normal*incoming;
          }
        const auto center=closestOnSegment(vertex.mPosition,from,to);
        auto delta=vertex.mPosition-center;
        const float distanceSq=delta.LengthSq();
        if(distanceSq>=radius*radius) continue;
        const auto oldDelta=previous-closestOnSegment(previous,from,to);
        const auto normal=distanceSq>1.e-12f ? delta/std::sqrt(distanceSq) : oldDelta.NormalizedOr(JPH::Vec3::sAxisZ());
        vertex.mPosition=center+normal*radius;
        const float incoming=vertex.mVelocity.Dot(normal);
        if(incoming<0.f) vertex.mVelocity-=normal*incoming;
        }
      }
    body.maxSpeed=0;
    body.maxSpeedVertex=uint32_t(-1);
    body.maxSpeedContact=false;
    body.maxSpeedContactBody=JPH::BodyID();
    body.maxSpeedContactNormal=Tempest::Vec3();
    body.contactCount=body.contacts.size();
    bool invalid=false;
    for(size_t i=0;i<motion.GetVertices().size();++i) {
      const auto& vertex=motion.GetVertex(uint32_t(i));
      const float speed=vertex.mVelocity.Length()*100.f;
      invalid|=!std::isfinite(speed) || vertex.mPosition.IsNaN() || (vertex.mPosition-body.rest[i]).LengthSq()>10000.f;
      if(std::isfinite(speed) && speed>body.maxSpeed) {
        body.maxSpeed=speed;
        body.maxSpeedVertex=uint32_t(i);
        }
      if(body.pinned[i]) body.maxAnchorError=std::max(body.maxAnchorError,(vertex.mPosition-body.rest[i]).Length()*100.f);
      }
    if(body.maxSpeedVertex!=uint32_t(-1)) {
      const auto& vertex=motion.GetVertex(body.maxSpeedVertex);
      const auto transform=native.GetCenterOfMassTransform();
      body.maxSpeedSource=project(body.inverse,body.translation()+CollisionWorld::toCentimeters(body.rest[body.maxSpeedVertex]));
      body.maxSpeedWorld=CollisionWorld::toCentimeters(JPH::Vec3(transform*vertex.mPosition));
      for(const auto& contact:body.contacts)
        if(contact.vertex==body.maxSpeedVertex) {
          body.maxSpeedContact=true;
          body.maxSpeedContactBody=contact.body;
          body.maxSpeedContactNormal=contact.normal;
          break;
          }
      }
    if(body.maxSpeed>body.maxSpeedEver) {
      body.maxSpeedEver=body.maxSpeed;
      body.maxSpeedEverVertex=body.maxSpeedVertex;
      body.maxSpeedEverSource=body.maxSpeedSource;
      body.maxSpeedEverWorld=body.maxSpeedWorld;
      body.maxSpeedEverContact=body.maxSpeedContact;
      body.maxSpeedEverContactNormal=body.maxSpeedContactNormal;
      body.maxSpeedEverContactBody=body.maxSpeedContactBody;
      }
    if(invalid) {
      // Recover on the next main-thread frame by removing the native body.
      // No unchecked vertex ever reaches the renderer.
      ++body.recoveryCount;
      body.recover=true;
      body.simulated=body.source;
      return;
      }
    for(size_t i=0;i<body.source.size();++i) {
      const auto world=CollisionWorld::toCentimeters(JPH::Vec3(native.GetCenterOfMassTransform()*motion.GetVertex(body.mapping[i]).mPosition));
      body.simulated[i]=project(body.inverse,world);
      }
    });
  }
void Cloth::finishFrame() {
  FrameProfile::Scope timing(FrameProfile::Zone::Cloth);
  constexpr double step=1.0/30.0;
  impl->each([&](Instance::Impl& body) {
    // Hidden time is not accumulated. Keep both native state and the render
    // interpolation phase so returning to the camera resumes the last pose.
    if(body.visible && !body.body.IsInvalid()) body.accumulated+=impl->frameSeconds;
    });
  for(int iteration=0;iteration<2;++iteration) {
    bool any=false;
    impl->each([&](Instance::Impl& body) {
      body.stepPending=body.visible && !body.body.IsInvalid() && body.accumulated+1.e-9>=step;
      if(!body.stepPending) return;
      body.accumulated=std::max(0.0,body.accumulated-step);
      body.previous=body.simulated;
      any=true;
      });
    if(!any) break;
    prepareStep(float(step));
    finishStep();
    }
  impl->each([&](Instance::Impl& body) {
    if(body.accumulated>=step) body.accumulated=std::fmod(body.accumulated,step);
    // A native fallback animates itself. Hidden cloth keeps its last uploaded
    // mesh; toggles and radius blending still update the views immediately.
    if(!body.dirty && (!body.visible || body.weight<=0.f)) return;
    const float alpha=float(std::clamp(body.accumulated/step,0.0,1.0));
    for(size_t i=0;i<body.rendered.size();++i) {
      const auto simulated=body.previous[i]+(body.simulated[i]-body.previous[i])*alpha;
      if(body.weight>=1.f) body.rendered[i]=simulated;
      else {
        const auto reference=body.reference(i);
        body.rendered[i]=reference+(simulated-reference)*body.weight;
        }
      }
    if(body.update) body.update(body.rendered,body.weight);
    body.dirty=false;
    });
  impl->frameSeconds=0;
  }

std::vector<Cloth::Snapshot> Cloth::snapshots() const {
  std::vector<Snapshot> result;
  for(const auto& weak:impl->instances) {
    const auto instance=weak.lock();
    if(!instance) continue;
    const auto& body=*instance->impl;
    Snapshot snapshot;
    snapshot.id=reinterpret_cast<uintptr_t>(instance.get());
    snapshot.zenId=body.zenId;
    snapshot.visual=body.visual;
    snapshot.center=body.center();
    snapshot.active=instance->active();
    snapshot.exposed=body.exposed;
    snapshot.visible=body.visible;
    snapshot.simulationSteps=body.simulationSteps;
    snapshot.vertices=body.pinned.size();
    snapshot.pinned=size_t(std::count(body.pinned.begin(),body.pinned.end(),true));
    snapshot.simulationWeight=body.weight;
    snapshot.maxAnchorErrorCm=body.maxAnchorError;
    snapshot.maxSpeedCmPerSec=body.maxSpeed;
    snapshot.maxSpeedEverCmPerSec=body.maxSpeedEver;
    snapshot.contactVertices=body.contactCount;
    snapshot.maxSpeedVertex=body.maxSpeedVertex;
    snapshot.maxSpeedSourceCm=body.maxSpeedSource;
    snapshot.maxSpeedWorldCm=body.maxSpeedWorld;
    snapshot.maxSpeedContact=body.maxSpeedContact;
    snapshot.maxSpeedContactNormal=body.maxSpeedContactNormal;
    snapshot.maxSpeedEverVertex=body.maxSpeedEverVertex;
    snapshot.maxSpeedEverSourceCm=body.maxSpeedEverSource;
    snapshot.maxSpeedEverWorldCm=body.maxSpeedEverWorld;
    snapshot.maxSpeedEverContact=body.maxSpeedEverContact;
    snapshot.maxSpeedEverContactNormal=body.maxSpeedEverContactNormal;
    snapshot.windOccluders=body.windOccluders;
    const auto label=[&](JPH::BodyID id) -> std::string {
      if(id.IsInvalid()) return {};
      JPH::BodyLockRead lock(impl->system().GetBodyLockInterface(),id);
      if(!lock.Succeeded()) return {};
      const auto* collider=reinterpret_cast<const CollisionWorld::CollisionBody*>(lock.GetBody().GetUserData());
      return collider!=nullptr ? collider->label() : std::string();
      };
    std::set<std::string> contactLabels;
    std::set<uint32_t> contactIds;
    for(const auto& contact:body.contacts)
      if(contactIds.insert(contact.body.GetIndexAndSequenceNumber()).second)
        contactLabels.insert(label(contact.body));
    snapshot.contactBodies.assign(contactLabels.begin(),contactLabels.end());
    snapshot.maxSpeedContactBody=label(body.maxSpeedContactBody);
    snapshot.maxSpeedEverContactBody=label(body.maxSpeedEverContactBody);
    snapshot.transitionMaxDeltaCm=body.transitionMaxDelta;
    snapshot.projectileHits=body.projectileHits;
    snapshot.arrowPasses=body.arrowPasses;
    snapshot.recoveryCount=body.recoveryCount;
    snapshot.positionsCm=body.rendered;
    snapshot.indices=body.indices;
    for(size_t i=0;i<body.rendered.size();++i) {
      snapshot.worldPositionsCm.push_back(project(body.matrix,body.rendered[i]));
      snapshot.referenceWorldPositionsCm.push_back(project(body.matrix,body.reference(i)));
      if(body.pinned[body.mapping[i]]) snapshot.anchorsCm.push_back(project(body.matrix,body.source[i]));
      }
    result.push_back(std::move(snapshot));
    }
  return result;
  }
void Cloth::resetStats() {
  impl->each([](Instance::Impl& body) {
    body.maxAnchorError=body.maxSpeedEver=body.transitionMaxDelta=0;
    body.maxSpeedEverVertex=uint32_t(-1);
    body.maxSpeedEverSource=body.maxSpeedEverWorld=body.maxSpeedEverContactNormal=Tempest::Vec3();
    body.maxSpeedEverContact=false;
    body.maxSpeedEverContactBody=JPH::BodyID();
    body.projectileHits=body.arrowPasses=body.recoveryCount=0;
    });
  }
size_t Cloth::projectile(const Tempest::Vec3& from,const Tempest::Vec3& to,const Tempest::Vec3& kick,bool arrow) {
  if(!impl->enabled || !finite(kick) || !finite(from) || !finite(to)) return 0;
  size_t hitCount=0;
  impl->each([&](Instance::Impl& body) {
    if(body.body.IsInvalid() || body.weight<=0.f) return;
    std::set<uint32_t> touched;
    for(size_t i=0;i+2<body.indices.size();i+=3) {
      const auto a=body.indices[i],b=body.indices[i+1],c=body.indices[i+2];
      if(!segmentTriangle(from,to,project(body.matrix,body.rendered[a]),project(body.matrix,body.rendered[b]),project(body.matrix,body.rendered[c]))) continue;
      touched.insert(body.mapping[a]); touched.insert(body.mapping[b]); touched.insert(body.mapping[c]);
      }
    if(touched.empty()) return;
    JPH::BodyLockWrite lock(impl->system().GetBodyLockInterface(),body.body);
    if(!lock.Succeeded()) return;
    auto& motion=*static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    const auto velocity=CollisionWorld::toMeters(kick*100.f);
    for(auto index:touched) {
      auto& vertex=motion.GetVertex(index);
      if(vertex.mInvMass>0.f) vertex.mVelocity=limited(vertex.mVelocity+velocity);
      }
    ++body.projectileHits;
    if(arrow) ++body.arrowPasses;
    ++hitCount;
    });
  return hitCount;
  }
size_t Cloth::impulse(const Tempest::Vec3& center,float radius,const Tempest::Vec3& kick) {
  if(!impl->enabled || radius<=0.f || !std::isfinite(radius) || !finite(center) || !finite(kick)) return 0;
  size_t count=0;
  impl->each([&](Instance::Impl& body) {
    if(body.body.IsInvalid()) return;
    JPH::BodyLockWrite lock(impl->system().GetBodyLockInterface(),body.body);
    if(!lock.Succeeded()) return;
    auto& native=lock.GetBody();
    auto& motion=*static_cast<JPH::SoftBodyMotionProperties*>(native.GetMotionProperties());
    bool hit=false;
    for(auto& vertex:motion.GetVertices()) {
      if(vertex.mInvMass<=0.f) continue;
      const auto world=CollisionWorld::toCentimeters(JPH::Vec3(native.GetCenterOfMassTransform()*vertex.mPosition));
      const float distance=(world-center).length();
      if(distance>=radius) continue;
      vertex.mVelocity=limited(vertex.mVelocity+CollisionWorld::toMeters(kick*100.f)*(1.f-distance/radius));
      hit=true;
      }
    if(hit) { ++count; ++body.projectileHits; }
    });
  return count;
  }
