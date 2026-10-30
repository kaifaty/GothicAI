#include "dynamicworld.h"

#include "collisionworld.h"
#include "physicmeshshape.h"
#include "physicvbo.h"
#include "ragdoll.h"
#include "cloth.h"
#include "worldwind.h"
#include "gothic.h"
#include "arrowtrace.h"
#include "frameprofile.h"
#include <Tempest/Log>
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Geometry/ClosestPoint.h>

#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstddef>
#include <list>
#include <map>
#include <unordered_map>
#include <cctype>
#include <chrono>
#include <unordered_set>
#include <stdexcept>

#include "graphics/mesh/submesh/packedmesh.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/bullet.h"
#include "world/world.h"
#include "game/serialize.h"
#include "game/physicssave.h"
#include "game/gamescript.h"

#include "utils/dbgpainter.h"

const float DynamicWorld::worldHeight = 20000; //TODO: remove

namespace {
constexpr uint32_t landscapeMask = (1u << DynamicWorld::C_Landscape) | (1u << DynamicWorld::C_Object) |
                                    (1u << DynamicWorld::C_MovableQuery);
constexpr uint32_t visualMask    = landscapeMask | (1u << DynamicWorld::C_SmallVobQuery);
constexpr uint32_t waterMask     = 1u << DynamicWorld::C_Water;

bool isNpcMoveSkipped(const Tempest::Vec3& displacement) {
  constexpr float eps = 2.f; // Legacy component-wise two centimetre skip.
  return std::abs(displacement.x)<eps && std::abs(displacement.y)<eps && std::abs(displacement.z)<eps;
  }

ItemMaterial impactMaterial(asset::MaterialGroup material) {
  switch(material) {
    case asset::MaterialGroup::METAL: return MAT_METAL;
    case asset::MaterialGroup::STONE: return MAT_STONE;
    case asset::MaterialGroup::WOOD:  return MAT_WOOD;
    default:                          return MAT_CLAY;
    }
  }

float vobBuoyancyFraction(asset::MaterialGroup material, std::string_view className = {}) {
  if(material!=asset::MaterialGroup::WOOD)
    return DynamicWorld::materialDensity(material)/1000.f;
  // Water displacement is independent of native push mass and its CSV multipliers.
  // Classless wood, including broken pieces, uses the solid-wood target.
  if(className=="barrel") return 1.f/3.f;
  if(className=="crate") return .5f;
  if(className=="table" || className=="cupboard") return .9f;
  return .6f;
  }

double volumeScale(const Tempest::Matrix4x4& matrix) {
  const auto* m = matrix.data();
  return std::abs(double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
                  double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
                  double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2]));
  }

bool noticeableMovableTrace(const Tempest::Matrix4x4& origin, const Tempest::Matrix4x4& current, bool& tilted) {
  double originLength=0.,currentLength=0.,dot=0.;
  for(int component=0;component<3;++component) {
    const double a=origin.at(1,component),b=current.at(1,component);
    originLength+=a*a;
    currentLength+=b*b;
    dot+=a*b;
    }
  // Compare the original and current local up axes, so rotating around the
  // upright axis cannot turn a table in place into a fallen-table trace.
  // Float matrices and the native quaternion roundtrip can put exactly 30
  // degrees a few ulps over the boundary.
  tilted = originLength>0. && currentLength>0. && dot/std::sqrt(originLength*currentLength)<0.8660254037844386-1.e-6;
  double displacement=0.;
  for(int component=0;component<3;++component) {
    const double delta=double(current.at(3,component))-origin.at(3,component);
    displacement+=delta*delta;
    }
  return displacement>50.*50. || tilted;
  }

DynamicWorld::RayLandResult toResult(const CollisionWorld::QueryResult& hit, const Tempest::Vec3& to) {
  DynamicWorld::RayLandResult ret = {};
  ret.hasCol      = hit.hasHit;
  ret.v           = to;
  ret.hitFraction = hit.fraction;

  if(!hit.hasHit) {
    ret.v.y = -std::numeric_limits<float>::infinity();
    return ret;
    }

  ret.v      = hit.point;
  ret.mat    = hit.material;
  ret.sector = hit.sector;
  if(hit.body==nullptr)
    return ret;

  if(hit.body->category()==DynamicWorld::C_Landscape)
    ret.n = hit.normal;
  // Object normals stay zero: the movement code relies on this for sliding.
  if(hit.body->ptrType()==DynamicWorld::P_Editor)
    ret.uptr = hit.body->payload();
  else if(hit.body->category()==DynamicWorld::C_Object && hit.body->ptrType()==DynamicWorld::P_Interactive)
    ret.vob = reinterpret_cast<Interactive*>(hit.body->payload());
  return ret;
  }
}

struct DynamicWorld::MovableVobs {
  struct Part {
    const PhysicMeshShape* source = nullptr;
    Tempest::Matrix4x4    local;
    };
  struct Shape {
    JPH::ShapeRefC              hull;
    std::vector<Part>           parts;
    asset::MaterialGroup      material = asset::MaterialGroup::UNDEFINED;
    float                      friction = 0.f;
    bool                       hullAttempted = false;
    std::shared_ptr<const FractureMesh> fracture;
    };
  std::unordered_map<std::string,std::shared_ptr<Shape>> shapes;
  std::vector<BodyData*> bodies;
  std::vector<MovableVobContacts> census;
  double validationMs = 0;
  bool finalized = false;
  bool enabled = true;
  bool destructionEnabled = true;
  };

struct DynamicWorld::ItemHulls {
  std::unordered_map<std::string,ItemHull> shapes;
  };

struct DynamicWorld::BodyData {
  ~BodyData() {
    if(movableOwner!=nullptr) {
      auto& bodies = movableOwner->bodies;
      bodies.erase(std::remove(bodies.begin(),bodies.end(),this),bodies.end());
      }
    }
  std::unique_ptr<CollisionWorld::CollisionBody> body;
  MovableVobs* movableOwner = nullptr;
  std::shared_ptr<MovableVobs::Shape> movableShape;
  std::vector<std::unique_ptr<CollisionWorld::CollisionBody>> query;
  Tempest::Matrix4x4 origin, current;
  std::shared_ptr<MovableVobPose> pose = std::make_shared<MovableVobPose>();
  std::string visual, className;
  std::string disposition;
  float movableMass = 0.f, massMultiplier = 1.f;
  bool blocksWalking = false, legacyCollision = true;
  bool wasSleeping = true;
  Tempest::Vec3 localBounds[2];
  bool trace = false, tilted = false;
  int64_t traceTime = -1;
  std::string traceCulprit;
  std::function<void(const Tempest::Matrix4x4&)> transformCallback;
  const FractureRule* fractureRule = nullptr;
  float health = 0, breakImpulse = 0;
  Tempest::Vec3 breakPoint;
  bool pendingBreak = false, fragmentsWereSleeping = true;
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
  std::vector<int64_t> fragmentSleepTime;
  std::vector<bool> fragmentPermanent;
  std::function<void(bool,const std::vector<Tempest::Matrix4x4>&)> fractureCallback;
  };

struct DynamicWorld::WorldBodies {
  std::vector<JPH::Vec3>                        vertices;
  std::vector<JPH::Vec3>                        clothVertices;
  std::unique_ptr<CollisionWorld::CollisionBody> landscape;
  std::unique_ptr<CollisionWorld::CollisionBody> water;
  std::unique_ptr<CollisionWorld::CollisionBody> originalCloth;
  };

struct DynamicWorld::BBoxShape {
  std::unique_ptr<CollisionWorld::CollisionBody> body;
  };

struct DynamicWorld::NpcBody {
  NpcBody(float radius, float height):radius(radius),cylinderHeight(height) {
    if(radius>0.f) {
      if(height>0.f)
        shape = new JPH::CapsuleShape(CollisionWorld::toMeters(height*0.5f), CollisionWorld::toMeters(radius));
      else
        shape = new JPH::SphereShape(CollisionWorld::toMeters(radius));
      }
    }

  JPH::ShapeRefC shape;
  std::unique_ptr<CollisionWorld::VirtualCharacter> virtualCharacter;
  bool virtualDiagnosticsEnabled = false;
  std::unique_ptr<CharacterVirtualDiagnostics::NpcMove> virtualDiagnosticMove;
  void*         userPointer = nullptr;
  float         radius      = 0.f;
  float         cylinderHeight = 0.f;
  Tempest::Vec3 pos      = {};
  float         h        = 0;
  float         gPadd    = 0.f;
  float         stepSz   = 0.f;
  bool          enable   = true;
  size_t        frozen   = size_t(-1);
  uint64_t      lastMove = 0;

  // Relevant for npc-to-npc collision; Jolt does not move these ellipsoids.
  float         angle    = 0;
  Tempest::Vec3 bboxCen  = {};
  Tempest::Vec3 bboxSize = {};
  float         maxRXZ   = 0;
  float         maxRY    = 0;

  float         maxR     = 0;

  Tempest::Vec3 offsetCenter(float c, float s) const {
    auto off = bboxCen;
    return Tempest::Vec3(off.x*c - off.z*s,
                         off.y,
                         off.x*s + off.z*c);
    }

  float ellipsoidRadius(Tempest::Vec3 dir, float c, float s) const {
    s = -s; // need to cancel object rotation

    Tempest::Vec3 radius = bboxSize;
    dir = Tempest::Vec3(dir.x*c - dir.z*s,
                        dir.y,
                        dir.x*s + dir.z*c);
    return (dir * radius).length();
    }

  Npc* toNpc() {
    return reinterpret_cast<Npc*>(userPointer);
    }

  auto groundOffset() const {
    const float extPadding = 15.f; // Khorinis port hack
    return h*0.5f + gPadd*0.5f + extPadding;
    }

  auto centerPosition() const {
    float padd = groundOffset();
    return Tempest::Vec3(pos.x, pos.y+padd, pos.z);
    }

  void setPosition(const Tempest::Vec3& p) {
    pos = p;
    }
  };

struct DynamicWorld::NpcBodyList final {
  struct Record final {
    NpcBody* body = nullptr;
    float    x    = 0.f;
    };

  NpcBodyList(DynamicWorld& wrld):wrld(wrld){
    body  .reserve(1024);
    frozen.reserve(1024);
    }

  NpcBody* create(const Tempest::Vec3 &min, const Tempest::Vec3 &max) {
    auto  size    = max - min;
    float radius  = std::min(size.y*0.5f, std::min(size.x, size.z))*0.5f; // npc-to-landscape collision size
    float height  = size.y;

    // ghostPadding = 90;
    float ghostPadding = height*0.5f;
    float cHeight      = std::max(height-2.f*radius-ghostPadding, 0.f);

    // The custom movement controller queries this capsule without registering a simulated body.
    NpcBody* obj = new NpcBody(radius, cHeight);

    obj->h      = height;
    obj->gPadd  = ghostPadding;

    obj->bboxCen  = (max+min)*0.5f;
    obj->bboxSize = (max-min)*0.5f;

    // safe tunneling size
    obj->stepSz   = radius;
    obj->stepSz   = std::min(obj->stepSz, std::abs(max.x));
    obj->stepSz   = std::min(obj->stepSz, std::abs(min.x));
    obj->stepSz   = std::min(obj->stepSz, std::abs(max.z));
    obj->stepSz   = std::min(obj->stepSz, std::abs(min.z));

    // searchable radius, where we use logica-position(obj->pos), instead of physical center
    obj->maxRXZ   = std::max(obj->maxRXZ, std::abs(max.x));
    obj->maxRXZ   = std::max(obj->maxRXZ, std::abs(min.x));
    obj->maxRXZ   = std::max(obj->maxRXZ, std::abs(max.z));
    obj->maxRXZ   = std::max(obj->maxRXZ, std::abs(min.z));
    obj->maxRY    = std::max(obj->maxRY,  std::abs(max.y));
    obj->maxRY    = std::max(obj->maxRY,  std::abs(min.y));

    maxRXZ = std::max(maxRXZ, obj->maxRXZ);
    maxRY  = std::max(maxRY,  obj->maxRY);

    obj->maxR = std::max(obj->maxRXZ, obj->maxRY);

    add(obj);
    return obj;
    }

  void add(NpcBody* b){
    Record r;
    r.body = b;
    r.x    = b->pos.x;
    body.push_back(r);
    }

  bool del(void* b) {
    if(del(b,body,false))
      return true;
    if(del(b,frozen,true))
      return true;
    return false;
    }

  bool del(void* b, std::vector<Record>& arr, bool preserveOrder) {
    for(size_t i=0;i<arr.size();++i){
      if(arr[i].body!=b)
        continue;
      if(preserveOrder) {
        arr.erase(arr.begin()+ptrdiff_t(i));
        } else {
        arr[i]=arr.back();
        arr.pop_back();
        }
      return true;
      }
    return false;
    }

  void onMove(NpcBody& n){
    if(n.frozen==size_t(-1)) {
      n.lastMove = tick;
      return;
      }

    auto&       fr = frozen[n.frozen];
    const float x  = fr.x;
    if((n.frozen==0 || frozen[n.frozen-1].x<x) &&
       (n.frozen+1==frozen.size() || x<frozen[n.frozen+1].x)) {
      // still ordered
      fr.x = fr.body->pos.x;
      return;
      }

    fr.body = nullptr;

    Record r;
    r.body = &n;
    body.push_back(r);

    n.lastMove = tick;
    n.frozen   = size_t(-1);
    }

  float raySphereTest(const Tempest::Vec3& origin, const Tempest::Vec3& dir, const Tempest::Vec3& sphere, float R) const {
    auto oc = origin - sphere;

    // Quadratic coefficients for (ray.origin + t*ray.direction - center)^2 = radius^2
    float a = 1.f; // Always 1 if direction is normalized
    float b = 2.0f * Tempest::Vec3::dotProduct(oc, dir);
    float c = Tempest::Vec3::dotProduct(oc,oc) - R*R;

    float discriminant = b * b - 4 * a * c;
    if(discriminant < 0)
      return -1;
    // Find the nearest intersection point (smallest positive t)
    float t0 = (-b - std::sqrt(discriminant)) / (2.0f * a);
    float t1 = (-b + std::sqrt(discriminant)) / (2.0f * a);

    if(t0 > 0)
      return t0;
    if(t1 > 0)
      return t1;
    return -1; // Intersection is behind the ray
    }

  float rayEllipseTest(const Tempest::Vec3& origin, const Tempest::Vec3& dir, const Tempest::Vec3& cen, const Tempest::Vec3& size) const {
    if(size.x<=0.001f || size.y<=0.001f || size.z<=0.001f)
      return -1.f;

    auto  ori = origin - cen;
    auto  oc  = Tempest::Vec3(ori.x/size.x, ori.y/size.y, ori.z/size.z);
    auto  dx  = Tempest::Vec3(dir.x/size.x, dir.y/size.y, dir.z/size.z);
    auto  ndx = Tempest::Vec3::normalize(dx);

    float t   = raySphereTest(oc, ndx, Tempest::Vec3(0.f), 1.f);
    if(t<=0)
      return t;

    auto hit = (ndx*t);
    hit.x *= size.x;
    hit.y *= size.y;
    hit.z *= size.z;

    return hit.length();
    }

  bool rayTest(NpcBody& npc, const Tempest::Vec3& s, const Tempest::Vec3& dir, float tMax, float extR, float& proj) {
    if(!npc.enable)
      return false;

    if(raySphereTest(s, dir, npc.pos, npc.maxR+extR)<0.f)
      return false;

    const float acos = std::cos(npc.angle), asin = std::sin(npc.angle);
    const auto  pos  = npc.pos + npc.offsetCenter(acos, asin);

    static float multiplyer = 2; // to address overly small bboxes
    const float t = rayEllipseTest(s, dir, pos, npc.bboxSize*multiplyer+extR);
    if(t<0.f || t>=tMax)
      return false;
    proj = t;
    return true;
    }

  auto rayTest(const Tempest::Vec3& s, const Tempest::Vec3& e, float extR, const Npc* except, float* distance = nullptr) {
    NpcBody* ret     = nullptr;
    auto     dir     = Tempest::Vec3::normalize(e-s);
    auto     tMax    = (e-s).length();
    float    tHit    = tMax;

    for(auto i:body) {
      float proj = 0;
      if(i.body->toNpc()!=except && rayTest(*i.body, s, dir, tMax, extR, proj)) {
        if(proj<tHit) {
          ret  = i.body;
          tHit = proj;
          }
        }
      }
    for(auto i:frozen) {
      float proj = 0;
      if(i.body!=nullptr && i.body->toNpc()!=except && rayTest(*i.body, s, dir, tMax, extR, proj)) {
        if(proj<tHit) {
          ret  = i.body;
          tHit = proj;
          }
        }
      }
    if(distance!=nullptr)
      *distance=tHit;
    return ret;
    }

  // the segment passes near the npc: within twice its box's reach of the box's middle (its bones may
  // stick out of the box: a head over a short collision box); proj: how far along
  bool nearTest(NpcBody& npc, const Tempest::Vec3& s, const Tempest::Vec3& dir, float tMax, float extR, float& proj) {
    if(!npc.enable)
      return false;
    const float acos = std::cos(npc.angle), asin = std::sin(npc.angle);
    const auto  cen  = npc.pos + npc.offsetCenter(acos, asin);
    const float r    = 2.f*npc.maxR + extR;
    const float t    = std::clamp(Tempest::Vec3::dotProduct(cen-s,dir), 0.f, tMax);
    if((s+dir*t-cen).quadLength()>r*r)
      return false;
    proj = t;
    return true;
    }

  // every npc the segment passes near, the nearest first, until f takes one (it looks at the bones)
  template<class F>
  bool rayTestAll(const Tempest::Vec3& s, const Tempest::Vec3& e, float extR, F f) {
    auto  dir  = Tempest::Vec3::normalize(e-s);
    auto  tMax = (e-s).length();
    std::vector<std::pair<float,NpcBody*>> hits;
    for(auto i:body) {
      float proj = 0;
      if(nearTest(*i.body, s, dir, tMax, extR, proj))
        hits.emplace_back(proj,i.body);
      }
    for(auto i:frozen) {
      float proj = 0;
      if(i.body!=nullptr && nearTest(*i.body, s, dir, tMax, extR, proj))
        hits.emplace_back(proj,i.body);
      }
    std::sort(hits.begin(),hits.end(),[](auto& a, auto& b){ return a.first<b.first; });
    for(auto& i:hits)
      if(f(*i.second->toNpc()))
        return true;
    return false;
    }

  bool hasCollision(const DynamicWorld::NpcItem& obj, Tempest::Vec3& normal, Npc*& npc, float& colDepth) {
    static bool disable=false;
    if(disable)
      return false;

    const NpcBody* pn = dynamic_cast<const NpcBody*>(obj.obj);
    if(pn==nullptr)
      return false;

    const NpcBody& n = *pn;
    if(hasCollision(n,frozen,normal,npc,colDepth,true))
      return true;
    return hasCollision(n,body,normal,npc,colDepth,false);
    }

  bool hasCollision(const NpcBody& n, const std::vector<Record>& arr, Tempest::Vec3& normal, Npc*& npc, float& colDepth, bool sorted) const {
    auto l = arr.begin();
    auto r = arr.end();

    if(sorted) {
      const float dX = maxRXZ + n.maxRXZ;
      l = std::lower_bound(arr.begin(),arr.end(),n.pos.x-dX,[](const Record& b,float x){ return b.x<x; });
      r = std::upper_bound(arr.begin(),arr.end(),n.pos.x+dX,[](float x,const Record& b){ return x<b.x; });
      }

    const auto dist = std::distance(l,r);
    if(dist<=1)
      return false;

    bool ret=false;
    for(;l!=r;++l){
      auto& v = (*l);
      if(v.body!=nullptr && v.body->enable && hasCollision(n,*v.body,normal,colDepth)) {
        npc = v.body->toNpc();
        ret = true;
        }
      }
    return ret;
    }

  bool hasCollision(const NpcBody& a, const NpcBody& b, Tempest::Vec3& normal, float& colDepth) const {
    const float objMaxRXZ = a.maxRXZ + b.maxRXZ;
    const float objMaxRY  = a.maxRY  + b.maxRY;

    if((Tempest::Vec2(a.pos.x,a.pos.z)-Tempest::Vec2(b.pos.x,b.pos.z)).quadLength() > objMaxRXZ*objMaxRXZ)
      return false;

    if(std::abs(a.pos.y-b.pos.y) > objMaxRY)
      return false;

    if(&a==&b)
      return false;

    const float acos = std::cos(a.angle), asin = std::sin(a.angle);
    const float bcos = std::cos(b.angle), bsin = std::sin(b.angle);

    const auto  apos = a.pos + a.offsetCenter(acos, asin);
    const auto  bpos = b.pos + b.offsetCenter(bcos, bsin);
    const auto  dir  = bpos - apos;

    auto dlen = dir.length();
    auto ndir = dlen==0 ? Tempest::Vec3(1,0,0) : Tempest::Vec3::normalize(dir);
    const float rA = a.ellipsoidRadius( ndir, acos, asin);
    const float rB = b.ellipsoidRadius(-ndir, bcos, bsin);

    if(dlen < rA + rB) {
      float depth = (rA + rB - dlen)*0.5f;
      colDepth = std::max(colDepth, depth);
      normal -= ndir*depth;
      return true;
      }
    return false;
    }

  void adjustSort() {
    std::sort(frozen.begin(),frozen.end(),[](Record& a,Record& b){
      return a.x < b.x;
      });
    for(size_t i=0; i<frozen.size(); ++i)
      frozen[i].body->frozen = i;
    }

  void tickAabbs() {
    for(size_t i=0;i<body.size();) {
      if(body[i].body->lastMove!=tick){
        auto b = body[i];
        body[i]=body.back();
        body.pop_back();

        b.x = b.body->pos.x;
        frozen.push_back(b);
        } else {
        ++i;
        }
      }

    for(size_t i=0;i<frozen.size();) {
      if(frozen[i].body==nullptr) {
        frozen[i]=frozen.back();
        frozen.pop_back();
        }
      else if(frozen[i].body->lastMove==tick){
        frozen[i].body->frozen=size_t(-1);
        body.push_back(frozen[i]);
        frozen[i]=frozen.back();
        frozen.pop_back();
        }
      else {
        ++i;
        }
      }

    adjustSort();
    tick++;
    }

  DynamicWorld&         wrld;
  std::vector<Record>   body, frozen;

  uint64_t              tick   = 0;
  float                 maxRXZ = 0;
  float                 maxRY  = 0;
  };

struct DynamicWorld::BulletsList final {
  BulletsList(DynamicWorld& wrld):wrld(wrld){
    }

  BulletBody* add(BulletCallback* cb) {
    BulletBody b(&wrld,cb);
    body.push_front(std::move(b));
    return &body.front();
    }

  void del(BulletBody* b) {
    for(auto i=body.begin(), e=body.end();i!=e;++i){
      if(&(*i)==b) {
        ArrowTrace::forget(*i);
        body.erase(i);
        return;
        }
      }
    }

  void tick(uint64_t dt) {
    for(auto& i:body) {
      if(i.worldStopped) continue;
      wrld.moveBullet(i,i.dir,dt);
      if(i.cb!=nullptr)
        i.cb->onMove();
      }
    }

  void onMoveNpc(NpcBody& npc, NpcBodyList& list){
    for(auto& i:body) {
      float proj = 0;
      auto  dp = i.pos - i.lastPos; // move is symetrical: move of npc and move of projectile is the same
      if(i.cb==nullptr || i.worldStopped)
        continue;
      const bool precise = i.cb->isPrecise();
      if(precise ? list.nearTest(npc, i.pos, Tempest::Vec3::normalize(dp), dp.length(), i.tgRange, proj)
                 : list.rayTest (npc, i.pos, Tempest::Vec3::normalize(dp), dp.length(), i.tgRange, proj)) {
        ArrowTrace::write(i,i.cb,"moving_npc",0,npc.toNpc());
        const bool hit = precise ? i.cb->onCollidePrecise(*npc.toNpc(),i.lastPos,i.pos) : i.cb->onCollide(*npc.toNpc());
        ArrowTrace::write(i,i.cb,hit ? "moving_npc_hit" : "moving_npc_pass",0,npc.toNpc());
        if(hit)
          i.cb->onStop();
        }
      }
    }

  std::list<BulletBody> body;
  DynamicWorld&         wrld;
  };

struct DynamicWorld::BBoxList final {
  BBoxList(DynamicWorld& wrld):wrld(wrld){
    }

  void add(BBoxBody* b) {
    body.emplace_back(b);
    }

  void del(BBoxBody* b) {
    for(auto i=body.begin(), e=body.end();i!=e;++i){
      if(*i==b) {
        body.erase(i);
        return;
        }
      }
    }

  BBoxBody* rayTest(const Tempest::Vec3& from, const Tempest::Vec3& to) {
    for(auto* i:body) {
      const auto& obj = *i->shape->body;
      const JPH::RayCast ray(CollisionWorld::toMeters(from-obj.position()), CollisionWorld::toMeters(to-from));
      JPH::RayCastSettings settings;
      settings.mTreatConvexAsSolid = false;
      JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hits;
      obj.shape()->CastRay(ray,settings,JPH::SubShapeIDCreator(),hits);
      if(hits.HadHit())
        return i;
      }
    return nullptr;
    }

  std::vector<BBoxBody*> body;
  DynamicWorld&          wrld;
  };

DynamicWorld::DynamicWorld(World* owner, const asset::WorldMesh& worldMesh, const asset::WorldMesh* originalCloth) {
  world.reset(new CollisionWorld());
  ownerWorld=owner;
  clothWorld=std::make_unique<Cloth>(*world);
  bodies.reset(new WorldBodies());
  movableVobs.reset(new MovableVobs());
  itemHulls=std::make_unique<ItemHulls>();
  world->setWaterSurfaceCallback([this](const Tempest::Vec3& center,float bottom,float& surface) {
    // Start below the entire envelope: the centre of a floating barrel is
    // usually above water. Only solids can separate an underwater body from
    // the lake above a cave; the body's own movable query mesh cannot.
    constexpr uint32_t solids=(1u<<C_Landscape)|(1u<<C_Object);
    const auto water=implWaterRay(center,center+Tempest::Vec3(0,worldHeight,0),
                                 std::max(0.f,center.y-bottom)+.1f,solids);
    surface=water.wdepth;
    return water.hasCol;
    });
  world->setBodyImpactCallback([this](CollisionWorld::CollisionBody& body,float impulse,const Tempest::Vec3& point,
                                     std::string_view culprit) {
    if(body.ptrType()==P_MovableVob && body.payload()!=nullptr)
      damageMovableVob(*static_cast<BodyData*>(body.payload()),impulse,point,false,culprit);
    });

  {
  PackedMesh pkg(worldMesh,PackedMesh::PK_Physic);
  sectors.resize(pkg.subMeshes.size());
  for(size_t i=0;i<sectors.size();++i)
    sectors[i] = pkg.subMeshes[i].material.name;

  bodies->vertices.resize(pkg.vertices.size());
  for(size_t i=0;i<pkg.vertices.size();++i) {
    auto v = pkg.vertices[i];
    bodies->vertices[i] = CollisionWorld::toMeters(Tempest::Vec3(v.pos[0],v.pos[1],v.pos[2]));
    }

  landMesh .reset(new PhysicVbo(&bodies->vertices));
  waterMesh.reset(new PhysicVbo(&bodies->vertices));

  for(size_t i=0;i<pkg.subMeshes.size();++i) {
    auto& sm = pkg.subMeshes[i];
    if(!sm.material.disable_collision && sm.iboLength>0) {
      if(sm.material.group==asset::MaterialGroup::WATER) {
        waterMesh->addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group);
        } else {
        landMesh ->addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group,sectors[i].c_str());
        }
      }
    }
  }

  if(originalCloth!=nullptr && !originalCloth->polygons.material_indices.empty()) {
    PackedMesh pkg(*originalCloth,PackedMesh::PK_Physic);
    bodies->clothVertices.reserve(pkg.vertices.size());
    for(const auto& vertex:pkg.vertices)
      bodies->clothVertices.push_back(CollisionWorld::toMeters(Tempest::Vec3(vertex.pos[0],vertex.pos[1],vertex.pos[2])));
    originalClothMesh=std::make_unique<PhysicVbo>(&bodies->clothVertices);
    for(const auto& sub:pkg.subMeshes)
      if(!sub.material.disable_collision && sub.iboLength>0)
        originalClothMesh->addIndex(pkg.indices,sub.iboOffset,sub.iboLength,sub.material.group);
    }

  JPH::Vec3 bbox[2] = {JPH::Vec3::sZero(), JPH::Vec3::sZero()};
  Tempest::Matrix4x4 mt;
  mt.identity();
  if(!landMesh->isEmpty() && landMesh->shape()!=nullptr) {
    bodies->landscape = world->addCollisionBody(landMesh->shape(),mt,materialFriction(asset::MaterialGroup::NONE),
                                               C_Landscape,false,false,landMesh.get());
    bodies->landscape->setLabel("landscape");
    JPH::Vec3 min, max;
    bodies->landscape->getAabb(min,max);
    bbox[0] = JPH::Vec3::sMin(bbox[0],min);
    bbox[1] = JPH::Vec3::sMax(bbox[1],max);
    }

  if(!waterMesh->isEmpty() && waterMesh->shape()!=nullptr) {
    bodies->water = world->addCollisionBody(waterMesh->shape(),mt,0,C_Water,false,true,waterMesh.get());
    JPH::Vec3 min, max;
    bodies->water->getAabb(min,max);
    bbox[0] = JPH::Vec3::sMin(bbox[0],min);
    bbox[1] = JPH::Vec3::sMax(bbox[1],max);
    }

  world->setBBox(bbox[0],bbox[1]);
  npcList   .reset(new NpcBodyList(*this));
  bulletList.reset(new BulletsList(*this));
  bboxList  .reset(new BBoxList   (*this));

  if(owner!=nullptr) {
    world->setItemWakeCallback([owner](const Tempest::Vec3& min,const Tempest::Vec3& max,size_t budget) {
      const auto center=(min+max)*.5f;
      const float radius=(max-min).length()*.5f;
      size_t created=0;
      owner->detectItem(center,radius,[&](::Item& item) {
        const auto p=item.position();
        if(p.x<min.x || p.x>max.x || p.y<min.y || p.y>max.y || p.z<min.z || p.z>max.z)
          return;
        if(item.isDynamic() || created<budget)
          created+=item.wakePhysics() ? 1 : 0;
        });
      return created;
      });
    world->setItemHitCallback([owner](::Item& itm, asset::MaterialGroup mat, float impulse, float mass) {
      auto  snd = owner->addLandHitEffect(ItemMaterial(itm.handle().material),mat,itm.transform());
      float v   = impulse/mass;
      float vol = snd.volume()*std::min(v/10.f,1.f);
      snd.setVolume(vol);
      snd.play();
      });
    }
  }

DynamicWorld::~DynamicWorld(){
  for(auto* body:movableVobs->bodies)
    body->movableOwner = nullptr;
  }

Cloth& DynamicWorld::cloth() { return *clothWorld; }
const Cloth& DynamicWorld::cloth() const { return *clothWorld; }
void DynamicWorld::setClothEnabled(bool value) {
  clothWorld->setEnabled(value);
  if(ownerWorld!=nullptr && ownerWorld->view()!=nullptr)
    ownerWorld->view()->setOriginalClothVisible(!value);
  if(value) {
    bodies->originalCloth.reset();
    } else if(bodies->originalCloth==nullptr && originalClothMesh!=nullptr &&
              !originalClothMesh->isEmpty() && originalClothMesh->shape()!=nullptr) {
    bodies->originalCloth=world->addCollisionBody(originalClothMesh->shape(),Tempest::Matrix4x4::mkIdentity(),
                                                materialFriction(asset::MaterialGroup::NONE),
                                                C_Landscape,false,false,originalClothMesh.get());
    bodies->originalCloth->setLabel("cloth-fallback");
    }
  }
bool DynamicWorld::clothEnabled() const { return clothWorld->enabled(); }

DynamicWorld::RayLandResult DynamicWorld::landRay(const Tempest::Vec3& from, float maxDy) const {
  world->updateAabbs();
  if(maxDy==0)
    maxDy = worldHeight;
  const auto to = Tempest::Vec3(from.x,from.y-maxDy,from.z);
  return toResult(world->rayCast(from,to,landscapeMask,false),to);
  }

DynamicWorld::RayLandResult DynamicWorld::landSweep(const Tempest::Vec3& from, float R, float maxDy) const {
  world->updateAabbs();
  if(maxDy==0)
    maxDy = worldHeight;
  return sweep(from, Tempest::Vec3(from.x,from.y-maxDy,from.z), R);
  }

DynamicWorld::RayLandResult DynamicWorld::sweep(const Tempest::Vec3& from, const Tempest::Vec3& to, float R) const {
  return toResult(world->raySweep(from,to,R,landscapeMask,false), to);
  }

DynamicWorld::RayWaterResult DynamicWorld::waterRay(const Tempest::Vec3& from, float stepHeight) const {
  world->updateAabbs();
  return implWaterRay(from, Tempest::Vec3(from.x,from.y+worldHeight,from.z), stepHeight);
  }

DynamicWorld::RayWaterResult DynamicWorld::implWaterRay(const Tempest::Vec3& from, const Tempest::Vec3& to, float stepHeight, uint32_t solidMask) const {
  const auto sFrom = from-Tempest::Vec3(0,stepHeight,0);
  const auto hit = world->rayCast(sFrom,to,waterMask,false);

  RayWaterResult ret;
  if(hit.hasHit) {
    const float waterY = hit.point.y;
    const auto cave=world->rayCast(from,Tempest::Vec3(to.x,std::max(waterY,from.y),to.z),
                                   solidMask!=0 ? solidMask : visualMask,false);
    if(cave.hasHit && cave.point.y<waterY) {
      ret.wdepth = -std::numeric_limits<float>::infinity();
      ret.hasCol = false;
      } else {
      ret.wdepth = waterY;
      ret.hasCol = true;
      }
    return ret;
    }

  ret.wdepth = -std::numeric_limits<float>::infinity();
  ret.hasCol = false;
  return ret;
  }

DynamicWorld::RayCamResult DynamicWorld::cameraRay(const Tempest::Vec3& from, const Tempest::Vec3& to) const {
  std::vector<CollisionWorld::QueryResult> hits;
  world->rayCast(from,to,waterMask,false,&hits);
  RayCamResult ret;
  ret.waterCol = int(hits.size());
  return ret;
  }

DynamicWorld::RayLandResult DynamicWorld::ray(const Tempest::Vec3& from, const Tempest::Vec3& to) const {
  return toResult(world->rayCast(from,to,visualMask,false), to);
  }

DynamicWorld::RayLandResult DynamicWorld::rayClothCamera(const Tempest::Vec3& from,const Tempest::Vec3& to,
                                                       std::string_view ownVisual,const Tempest::Vec3* bounds,
                                                       size_t& selfHits,std::string* blockedLabel) const {
  std::vector<const CollisionWorld::CollisionBody*> excluded;
  const auto inside=[&](const Tempest::Vec3& point) {
    return point.x>=bounds[0].x-25.f && point.x<=bounds[1].x+25.f &&
           point.y>=bounds[0].y-25.f && point.y<=bounds[1].y+25.f &&
           point.z>=bounds[0].z-25.f && point.z<=bounds[1].z+25.f;
    };
  for(;;) {
    const auto hit=world->rayCast(from,to,visualMask,false,nullptr,&excluded);
    if(blockedLabel)*blockedLabel=hit.body?hit.body->label():"";
    if(!hit.hasHit || !hit.body || hit.body->category()!=C_Object || ownVisual.empty() ||
       hit.body->label()!=ownVisual || !inside(hit.point) || excluded.size()>=8)
      return toResult(hit,to);
    JPH::Vec3 low,high;hit.body->getAabb(low,high);
    const auto minimum=CollisionWorld::toCentimeters(low),maximum=CollisionWorld::toCentimeters(high);
    if(maximum.x<bounds[0].x-25.f || minimum.x>bounds[1].x+25.f ||
       maximum.y<bounds[0].y-25.f || minimum.y>bounds[1].y+25.f ||
       maximum.z<bounds[0].z-25.f || minimum.z>bounds[1].z+25.f)
      return toResult(hit,to);
    // Repeat the whole segment with only this exact native body excluded. A
    // wall behind or intersecting its hull must still reject the camera.
    excluded.push_back(hit.body);++selfHits;
    }
  }

DynamicWorld::RayQueryResult DynamicWorld::rayNpc(const Tempest::Vec3& from, const Tempest::Vec3& to, const Npc* except) const {
  RayQueryResult r;
  static_cast<RayLandResult&>(r) = ray(from,to);
  if(auto ptr = npcList->rayTest(from, (r.hasCol ? r.v : to), 1, except)) {
    r.npcHit = ptr->toNpc();
    r.hasCol = true;
    }
  return r;
  }

DynamicWorld::RayQueryResult DynamicWorld::rayArrow(const Tempest::Vec3& from, const Tempest::Vec3& to,
                                                    const std::function<bool(Npc&,const Tempest::Vec3&,const Tempest::Vec3&,float&)>& hit) const {
  RayQueryResult r;
  static_cast<RayLandResult&>(r) = ray(from,to);
  const Tempest::Vec3 end = r.hasCol ? r.v : to;
  npcList->rayTestAll(from,end,0,[&](Npc& npc) {
    float t = 0;
    if(!hit(npc,from,end,t))
      return false;
    r.npcHit = &npc;
    r.hasCol = true;
    r.v      = from + Tempest::Vec3::normalize(end-from)*t;
    return true;
    });
  return r;
  }

float DynamicWorld::soundOclusion(const Tempest::Vec3& from, const Tempest::Vec3& to) const {
  std::vector<CollisionWorld::QueryResult> hits;
  world->rayCast(from,to,visualMask | waterMask,true,&hits);
  const size_t count = hits.size();
  if(count<2)
    return 0;
  if(count>=16)
    return 1;

  std::sort(hits.begin(),hits.end(),[](const auto& a, const auto& b) { return a.fraction<b.fraction; });
  const float tlen = CollisionWorld::toMeters((from-to).length());
  float tr = 1.f;
  for(size_t i=1; i<count; i+=2)
    tr *= std::exp(-(hits[i].fraction-hits[i-1].fraction)*tlen/3.5f);
  return std::clamp(1.f-tr, 0.f, 1.f);
  }

DynamicWorld::NpcItem DynamicWorld::addGhostObj(const Skeleton* src) {
  Tempest::Vec3 min={0,0,0}, max={0,0,0};
  if(src != nullptr) {
    min = src->bboxCol[0];
    max = src->bboxCol[1];

    min.y += src->rootTr.y;
    max.y += src->rootTr.y;
    }
  auto obj = npcList->create(min,max);
  return NpcItem(this,obj);
  }

DynamicWorld::Item DynamicWorld::addStaticObj(const PhysicMeshShape* shape, const Tempest::Matrix4x4& m, std::string_view label) {
  if(shape==nullptr || shape->shape==nullptr)
    return Item();
  auto obj = std::make_unique<BodyData>();
  obj->body = world->addCollisionBody(shape->shape,m,shape->friction(),C_Object,false,false,&shape->mesh);
  obj->body->setLabel(label);
  obj->body->setNoiseProfile(impactMaterial(shape->material),shape->material,"vob");
  return Item(this,obj.release());
  }

DynamicWorld::Item DynamicWorld::addMovableObj(const PhysicMeshShape* shape, const Tempest::Matrix4x4& m, std::string_view label) {
  if(shape==nullptr || shape->shape==nullptr)
    return Item();
  auto obj = std::make_unique<BodyData>();
  obj->body = world->addCollisionBody(shape->shape,m,shape->friction(),C_Object,true,false,&shape->mesh);
  obj->body->setLabel(label);
  obj->body->setNoiseProfile(impactMaterial(shape->material),shape->material,"vob");
  return Item(this,obj.release());
  }

DynamicWorld::Item DynamicWorld::addDynamicObj(const Tempest::Matrix4x4& pos, const Bounds& b, asset::MaterialGroup mat) {
  auto extent = CollisionWorld::toMeters(b.bbox[1]-b.bbox[0]);
  const auto* m=pos.data();
  const double volumeScale=std::abs(double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
                                    double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
                                    double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2]));
  // Use the visual bounds' volume, before the minimum collision proxy padding.
  const float mass = float(materialDensity(mat)*double(extent.GetX())*extent.GetY()*extent.GetZ()*volumeScale);
  const auto halfExtent = JPH::Vec3::sMax(extent*0.5f,JPH::Vec3::sReplicate(0.15f));
  const auto center=CollisionWorld::toMeters((b.bbox[0]+b.bbox[1])*.5f);
  JPH::ShapeRefC shape = new JPH::RotatedTranslatedShape(center,JPH::Quat::sIdentity(),new JPH::BoxShape(halfExtent));
  auto obj = std::make_unique<BodyData>();
  obj->body = world->addDynamicBody(shape,pos,materialFriction(mat),mass);
  // Collision padding keeps small items easy to touch. Water displaces the
  // actual visual envelope so padding cannot make a metal sword float.
  // The native COM is already at the visual bounds' centre; the unpadded
  // box shares that centre while the body's transform keeps the visual pivot.
  const auto waterHalfExtent=JPH::Vec3::sMax(extent*.5f,JPH::Vec3::sReplicate(.0005f));
  obj->body->setBuoyancyShape(new JPH::BoxShape(waterHalfExtent,0.f));
  obj->body->setNoiseProfile(impactMaterial(mat),mat,"item");
  return Item(this,obj.release());
  }

const DynamicWorld::ItemHull& DynamicWorld::itemHullShape(const ProtoMesh& proto, std::string_view visual) {
  std::string key(visual);
  std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  auto [cached,inserted]=itemHulls->shapes.try_emplace(key);
  auto& hull=cached->second;
  if(!inserted)
    return hull;
  const auto extent=CollisionWorld::toMeters(proto.bbox()[1]-proto.bbox()[0]);
  hull.boundsVolume=extent.GetX()*extent.GetY()*extent.GetZ();
  proto.appendItemVertices(hull.vertices);
  std::vector<JPH::Vec3> points;
  points.reserve(hull.vertices.size());
  for(const auto& point:hull.vertices) {
    if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      hull.fallbackReason="nonfinite mesh vertex";
      return hull;
      }
    points.push_back(CollisionWorld::toMeters(point));
    }
  std::sort(points.begin(),points.end(),[](const auto& a,const auto& b) {
    if(a.GetX()!=b.GetX()) return a.GetX()<b.GetX();
    if(a.GetY()!=b.GetY()) return a.GetY()<b.GetY();
    return a.GetZ()<b.GetZ();
    });
  points.erase(std::unique(points.begin(),points.end()),points.end());
  if(points.size()<4) {
    hull.fallbackReason="fewer than four mesh vertices";
    return hull;
    }
  // Find the mesh's broad plane, including tilted leaves and scrolls. A flat
  // surface gets 4 mm of symmetric thickness, while a line stays a fallback.
  const auto origin=points.front();
  auto longest=JPH::Vec3::sZero();
  for(const auto& point:points)
    if((point-origin).LengthSq()>longest.LengthSq())
      longest=point-origin;
  auto normal=JPH::Vec3::sZero();
  for(const auto& point:points) {
    const auto candidate=longest.Cross(point-origin);
    if(candidate.LengthSq()>normal.LengthSq())
      normal=candidate;
    }
  if(longest.LengthSq()<=1.e-12f || normal.LengthSq()<=1.e-12f*longest.LengthSq()) {
    hull.fallbackReason="collinear mesh vertices";
    return hull;
    }
  normal=normal.Normalized();
  float low=std::numeric_limits<float>::infinity(),high=-low;
  for(const auto& point:points) {
    const float projection=normal.Dot(point-origin);
    low=std::min(low,projection);
    high=std::max(high,projection);
    }
  constexpr float minimumThickness=.004f;
  if(high-low<minimumThickness) {
    const auto padding=normal*((minimumThickness-(high-low))*.5f);
    const auto size=points.size();
    points.reserve(2*size);
    for(size_t i=0;i<size;++i) {
      points.push_back(points[i]+padding);
      points[i]-=padding;
      }
    }
  Physics::initialize();
  // Jolt subtracts this radius internally; it does not inflate the envelope.
  // A small hull tolerance preserves millimetre-scale detail on coins/leaves.
  JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.00025f);
  settings.mHullTolerance=.0001f;
  settings.mMaxErrorConvexRadius=.0001f;
  const auto result=settings.Create();
  if(result.HasError()) {
    hull.fallbackReason="Jolt: "+std::string(result.GetError().c_str());
    return hull;
    }
  const float volume=result.Get()->GetVolume();
  if(!std::isfinite(volume) || volume<=0.f) {
    hull.fallbackReason="zero or nonfinite hull volume";
    return hull;
    }
  const auto& shape=static_cast<const JPH::ConvexHullShape&>(*result.Get());
  std::vector<JPH::Vec3> directions;
  directions.reserve(14);
  for(int x=-1;x<=1;++x)
    for(int y=-1;y<=1;++y)
      for(int z=-1;z<=1;++z) {
        const int nonzero=int(x!=0)+int(y!=0)+int(z!=0);
        if(nonzero==1 || nonzero==3)
          directions.push_back(JPH::Vec3(float(x),float(y),float(z)).Normalized());
        }
  std::vector<JPH::Vec3> inflated;
  inflated.reserve(shape.GetNumPoints()*14);
  const auto inflate=[&](float padding) {
    inflated.clear();
    for(JPH::uint i=0;i<shape.GetNumPoints();++i) {
      const auto point=shape.GetPoint(i)+shape.GetCenterOfMass();
      for(const auto& direction:directions)
        inflated.push_back(point+direction*padding);
      }
    JPH::ConvexHullShapeSettings inflatedSettings(inflated.data(),int(inflated.size()),.00025f);
    inflatedSettings.mHullTolerance=.0001f;
    inflatedSettings.mMaxErrorConvexRadius=.0001f;
    return inflatedSettings.Create();
    };
  // One centimetre centres the visible envelope in Jolt's unchanged 2 cm slop.
  // Keep the source envelope for visible support measurements, mass and water.
  float padding=.01f;
  auto inflatedResult=inflate(padding);
  if(inflatedResult.HasError()) {
    hull.fallbackReason="Jolt inflated hull: "+std::string(inflatedResult.GetError().c_str());
    return hull;
    }
  // Inner radius is the shortest COM-to-support-plane distance, including
  // tilted thin meshes. Rebuilding also accounts for centroid movement.
  constexpr float minimumInnerRadius=.03f;
  if(inflatedResult.Get()->GetInnerRadius()<minimumInnerRadius) {
    float lower=padding,upper=.04f;
    auto upperResult=inflate(upper);
    for(int i=0;i<4 && !upperResult.HasError() && upperResult.Get()->GetInnerRadius()<minimumInnerRadius;++i) {
      lower=upper;upper*=2.f;upperResult=inflate(upper);
      }
    if(upperResult.HasError()) {
      hull.fallbackReason="Jolt inflated hull: "+std::string(upperResult.GetError().c_str());
      return hull;
      }
    if(upperResult.Get()->GetInnerRadius()<minimumInnerRadius) {
      hull.fallbackReason="inflated hull cannot reach the minimum inner radius";
      return hull;
      }
    for(int i=0;i<16 && upper-lower>.000001f;++i) {
      const float middle=(lower+upper)*.5f;
      auto middleResult=inflate(middle);
      if(middleResult.HasError()) {
        hull.fallbackReason="Jolt inflated hull: "+std::string(middleResult.GetError().c_str());
        return hull;
        }
      if(middleResult.Get()->GetInnerRadius()>=minimumInnerRadius) {
        upper=middle;upperResult=std::move(middleResult);
        }
      else lower=middle;
      }
    padding=upper;inflatedResult=std::move(upperResult);
    }
  hull.originalShape=result.Get();
  hull.shape=inflatedResult.Get();
  hull.volume=volume;
  hull.extraPaddingCm=(padding-.01f)*100.f;
  hull.vertices.clear();
  hull.vertices.reserve(shape.GetNumPoints());
  for(JPH::uint i=0;i<shape.GetNumPoints();++i)
    hull.vertices.push_back(CollisionWorld::toCentimeters(shape.GetPoint(i)+shape.GetCenterOfMass()));
  return hull;
  }

DynamicWorld::Item DynamicWorld::addDynamicObj(const Tempest::Matrix4x4& pos, const Bounds& bounds,
                                               asset::MaterialGroup mat, const ProtoMesh* proto,
                                               std::string_view visual, bool hulls) {
  if(!hulls || proto==nullptr)
    return addDynamicObj(pos,bounds,mat);
  const auto& hull=itemHullShape(*proto,visual);
  if(hull.shape==nullptr)
    return addDynamicObj(pos,bounds,mat);
  const float mass=std::max(.05f,float(materialDensity(mat)*hull.volume*volumeScale(pos)));
  auto obj=std::make_unique<BodyData>();
  obj->body=world->addDynamicBody(hull.shape,pos,materialFriction(mat),mass);
  // Express the original envelope in the inflated body's COM frame. Inflation
  // may shift the centroid of an asymmetric mesh, but must not move its water shape.
  obj->body->setBuoyancyShape(new JPH::OffsetCenterOfMassShape(hull.originalShape,
    hull.shape->GetCenterOfMass()-hull.originalShape->GetCenterOfMass()));
  // Dynamic bodies already use LinearCast, including the smallest items.
  obj->body->setNoiseProfile(impactMaterial(mat),mat,"item");
  return Item(this,obj.release());
  }

DynamicWorld::Item DynamicWorld::addMovableVob(const ProtoMesh& proto, std::string_view visual,
                                              const Tempest::Matrix4x4& matrix, float massMultiplier,
                                              bool blocksWalking, std::string_view className,
                                              bool legacyCollision, uint32_t zenId) {
  if(!std::isfinite(massMultiplier) || massMultiplier<=0.f)
    return {};
  std::string key(visual);
  std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  auto [cached,inserted] = movableVobs->shapes.try_emplace(key);
  if(inserted) {
    auto shape = std::make_shared<MovableVobs::Shape>();
    size_t largestAttach = 0;
    for(size_t i=0;i<proto.attach.size();++i) {
      const auto* source = proto.attach[i].shape.get();
      if(source==nullptr || source->shape==nullptr)
        continue;
      auto local = Tempest::Matrix4x4::mkIdentity();
      for(size_t node=0;node<proto.nodes.size();++node) {
        if(proto.nodes[node].attachId!=i)
          continue;
        if(proto.skeleton!=nullptr && node<proto.skeleton->tr.size()) {
          local = proto.skeleton->tr[node];
          }
        else {
          local = proto.nodes[node].transform;
          size_t parent = proto.nodes[node].parentId;
          // Nodes normally come from a Skeleton. Bound the fallback traversal
          // so malformed model hierarchy data cannot loop while loading a world.
          for(size_t depth=0;parent<proto.nodes.size() && depth<proto.nodes.size();++depth) {
            auto parentMatrix = proto.nodes[parent].transform;
            parentMatrix.mul(local);
            local = parentMatrix;
            parent = proto.nodes[parent].parentId;
            }
          }
        break;
        }
      const auto size = source->mesh.vertexCount();
      if(size>largestAttach) {
        largestAttach = size;
        shape->material = source->material;
        }
      shape->friction += source->friction();
      shape->parts.push_back({source,local});
      }
    if(!shape->parts.empty()) {
      shape->friction /= float(shape->parts.size());
      if(FractureRule::classify(visual)!=nullptr) {
        shape->fracture=FractureMesh::load(visual);
        if(shape->fracture==nullptr) {
          static bool warned=false;
          if(!warned) {
            warned=true;
            Tempest::Log::i("Destruction cache unavailable; uncached visuals keep their original physics (OPENGOTHIC_FRACTURE)");
            }
          }
        }
      cached->second = std::move(shape);
      }
    }
  if(cached->second==nullptr)
    return {};
  auto obj = std::make_unique<BodyData>();
  obj->movableShape = cached->second;
  obj->origin = obj->current = matrix;
  obj->visual = std::string(visual);
  obj->pose->zenId = zenId;
  obj->pose->visual = obj->visual;
  obj->pose->origin = matrix;
  obj->pose->transform = matrix;
  obj->className = std::string(className);
  obj->blocksWalking = blocksWalking;
  obj->legacyCollision = legacyCollision;
  obj->massMultiplier = massMultiplier;
  obj->fractureRule=FractureRule::classify(visual);
  obj->localBounds[0] = proto.bbox()[0];
  obj->localBounds[1] = proto.bbox()[1];
  rebuildMovableVob(*obj);
  if((movableVobs->enabled || className=="fence") && obj->body==nullptr)
    return {};
  obj->movableOwner = movableVobs.get();
  movableVobs->bodies.push_back(obj.get());
  return Item(this,obj.release());
  }

void DynamicWorld::prepareMovableVobShape(BodyData& obj) {
  auto& shape = *obj.movableShape;
  if(!shape.hullAttempted) {
    shape.hullAttempted = true;
    std::vector<JPH::Vec3> points;
    for(const auto& part:shape.parts)
      part.source->mesh.appendVertices(points,part.local);
    if(points.size()>=4) {
      Physics::initialize();
      // A single native hull closes cavities such as cup mouths and the space
      // below a table. Walking and aiming still use the original source faces.
      JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),0.001f);
      const auto result = settings.Create();
      if(!result.HasError() && result.Get()->GetVolume()>0.f)
        shape.hull = result.Get();
      }
    if(shape.hull==nullptr)
      Tempest::Log::e("Cannot build movable vob hull: ",obj.visual);
    }
  if(shape.hull!=nullptr)
    obj.movableMass = float(shape.hull->GetVolume()*volumeScale(obj.origin)*materialDensity(shape.material)*obj.massMultiplier);
  }

void DynamicWorld::rebuildMovableVob(BodyData& obj) {
  obj.body.reset();
  obj.query.clear();
  obj.fragments.clear();
  obj.fragmentSleepTime.clear();
  obj.fragmentPermanent.clear();
  obj.pendingBreak=false;
  obj.pose->broken=false;
  obj.health=obj.fractureRule!=nullptr ? obj.fractureRule->health : 0.f;
  obj.current = obj.origin;
  obj.wasSleeping = true;
  obj.trace = obj.tilted = false;
  obj.traceTime = -1;
  obj.traceCulprit.clear();
  auto& shape = *obj.movableShape;
  const bool fence=obj.className=="fence";
  if(movableVobs->enabled || fence)
    prepareMovableVobShape(obj);
  const bool allowed = obj.disposition.empty() || obj.disposition=="ok" || obj.disposition=="item";
  const bool enabled = (movableVobs->enabled || fence) && (allowed || fence) && shape.hull!=nullptr &&
                       std::isfinite(obj.movableMass) && obj.movableMass>0.f;
  if(enabled) {
    if(fence)
      // The authored mesh remains the exact static obstacle. This sensor hull
      // only owns destruction identity and the mass used by its future pieces.
      obj.body=world->addCollisionBody(shape.hull,obj.origin,shape.friction,C_MovableVob,false,true);
    else
      obj.body = world->addDynamicBody(shape.hull,obj.origin,shape.friction,obj.movableMass,C_MovableVob,true);
    }
  if(obj.body!=nullptr) {
    obj.body->setPayload(&obj,P_MovableVob);
    obj.body->setLabel(obj.visual);
    obj.body->setNoiseProfile(impactMaterial(shape.material),shape.material,obj.visual);
    obj.body->setBuoyancyFraction(vobBuoyancyFraction(shape.material,obj.className));
    }
  // cd_dynamic controls the original mesh queries independently of simulation.
  // The hull can move and receive impacts without becoming a new walking/ray obstacle.
  if(obj.legacyCollision) {
    const int category = !enabled || fence ? C_Object : obj.blocksWalking ? C_MovableQuery : C_SmallVobQuery;
    for(const auto& part:shape.parts) {
      auto matrix = obj.origin;
      matrix.mul(part.local);
      obj.query.push_back(world->addCollisionBody(part.source->shape,matrix,part.source->friction(),category,
                                                 enabled && !fence,enabled && !fence,&part.source->mesh));
      obj.query.back()->setPayload(&obj,P_MovableVob);
      obj.query.back()->setLabel(obj.visual);
      }
    }
  obj.pose->transform = obj.current;
  if(obj.transformCallback)
    obj.transformCallback(obj.current);
  if(obj.fractureCallback) obj.fractureCallback(false,{});
  }

void DynamicWorld::syncMovableVob(BodyData& obj) {
  if(obj.body==nullptr)
    return;
  const bool sleeping = obj.body->isSleeping();
  if(sleeping && obj.wasSleeping)
    return;
  obj.wasSleeping = sleeping;
  obj.current = obj.body->matrix();
  obj.pose->transform = obj.current;
  const bool trace = noticeableMovableTrace(obj.origin,obj.current,obj.tilted);
  if(trace && !obj.trace) {
    obj.traceTime = traceTime;
    obj.traceCulprit = obj.body->traceCulprit();
    }
  else if(!trace) {
    obj.traceTime = -1;
    obj.traceCulprit.clear();
    }
  obj.trace = trace;
  for(size_t i=0;i<obj.query.size();++i) {
    auto matrix = obj.current;
    matrix.mul(obj.movableShape->parts[i].local);
    obj.query[i]->setObjMatrix(matrix);
    }
  if(obj.transformCallback)
    obj.transformCallback(obj.current);
  }

void DynamicWorld::setMovableVobsEnabled(bool enabled) {
  if(movableVobs->enabled==enabled)
    return;
  movableVobs->enabled = enabled;
  for(auto* obj:movableVobs->bodies)
    if(obj->className!="fence" && !obj->pose->broken)
      rebuildMovableVob(*obj);
  if(enabled && movableVobs->finalized && movableVobs->census.empty())
    validateMovableVobs();
  }

void DynamicWorld::setBuoyancyEnabled(bool enabled) {
  world->setBuoyancyEnabled(enabled);
  }

void DynamicWorld::setItemWakeEnabled(bool enabled) {
  world->setItemWakeEnabled(enabled);
  }

bool DynamicWorld::buoyancyEnabled() const {
  return world->buoyancyEnabled();
  }

void DynamicWorld::setDestructionEnabled(bool enabled) {
  if(movableVobs->destructionEnabled==enabled) return;
  movableVobs->destructionEnabled=enabled;
  for(auto* obj:movableVobs->bodies) {
    obj->pendingBreak=false;
    obj->health=obj->fractureRule!=nullptr ? obj->fractureRule->health : 0.f;
    if(!enabled && obj->pose->broken) rebuildMovableVob(*obj);
    }
  }

bool DynamicWorld::destructionEnabled() const {
  return movableVobs->destructionEnabled;
  }

void DynamicWorld::finalizeMovableVobs() {
  movableVobs->finalized = true;
  // Capture the authored geometry even when starting with physics disabled:
  // a later enable must not classify against a moved door or a loaded save.
  const bool enabled = movableVobs->enabled;
  if(!enabled) {
    movableVobs->enabled = true;
    for(auto* obj:movableVobs->bodies)
      if(obj->className!="fence" && !obj->pose->broken)
        rebuildMovableVob(*obj);
    }
  validateMovableVobs();
  if(!enabled) {
    movableVobs->enabled = false;
    for(auto* obj:movableVobs->bodies)
      if(obj->className!="fence" && !obj->pose->broken)
        rebuildMovableVob(*obj);
    }
  }

double DynamicWorld::movableVobValidationMilliseconds() const {
  return movableVobs->validationMs;
  }

void DynamicWorld::validateMovableVobs() {
  const auto start = std::chrono::steady_clock::now();
  // All candidates still have their hulls at ZEN poses here. Decide the entire
  // set before replacing any hull with its original mesh: order cannot affect it.
  auto census = movableVobContacts(1.f);
  std::unordered_set<uint32_t> deepPairs;
  for(const auto& entry:census)
    for(const auto& c:entry.contacts)
      if(c.category==C_MovableVob && c.depth>4.f) {
        deepPairs.insert(entry.zenId);
        if(c.otherZenId!=uint32_t(-1))
          deepPairs.insert(c.otherZenId);
        }
  size_t rejected = 0;
  for(auto& entry:census) {
    const auto onTop = [&](const MovableVobContact& c) {
      return c.category==C_Object && c.side!=0 &&
             (c.otherMin.y-entry.hullMin.y)/std::max(entry.hullMax.y-entry.hullMin.y,1e-3f)>=.9f;
      };
    const auto size = [](const MovableVobContact& c) {
      const auto s = c.otherMax-c.otherMin;
      return std::max({s.x,s.y,s.z});
      };
    const MovableVobContact* deepest = nullptr;
    bool supported = false, carries = false, item = false;
    for(const auto& c:entry.contacts) {
      supported |= c.side==0;
      const bool top = onTop(c);
      const bool small = size(c)<=80.f;
      if(c.depth>4.f && !(top && small) && (deepest==nullptr || c.depth>deepest->depth))
        deepest = &c;
      carries |= top && !small;
      item |= top;
      }
    entry.disposition = !supported ? "hangs" : deepest!=nullptr ?
        (deepest->category==C_MovableVob ? "neighbour" : deepest->side==0 ? "floor" :
         deepest->category==C_Landscape ? "wall" : "structure") :
        carries ? "carries" : deepPairs.count(entry.zenId)!=0 ? "neighbour" : item ? "item" : "ok";
    entry.enabled = entry.disposition=="ok" || entry.disposition=="item";
    rejected += !entry.enabled;
    }
  // Preserve the exact origin geometry for the census, including rejected hulls.
  movableVobs->census = std::move(census);
  for(auto* obj:movableVobs->bodies)
    for(const auto& entry:movableVobs->census)
      if(obj->pose->zenId==entry.zenId) {
        obj->disposition = entry.disposition;
        if(!entry.enabled)
          rebuildMovableVob(*obj);
        break;
        }
  movableVobs->validationMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  Tempest::Log::i("movable vob geometry: ",movableVobs->census.size()," candidates, ",rejected,
                 " static, ",movableVobs->validationMs," ms");
  }

bool DynamicWorld::movableVobsEnabled() const {
  return movableVobs->enabled;
  }

void DynamicWorld::setPhysicsNoiseEnabled(bool enabled) {
  world->setPhysicsNoiseEnabled(enabled);
  }

bool DynamicWorld::physicsNoiseEnabled() const {
  return world->physicsNoiseEnabled();
  }

void DynamicWorld::setPhysicsNoiseCallback(std::function<void(const PhysicsNoise&)> callback) {
  world->setPhysicsNoiseCallback(std::move(callback));
  }

std::shared_ptr<const DynamicWorld::MovableVobPose> DynamicWorld::movableVobPose(uint32_t zenId) const {
  if(zenId==uint32_t(-1))
    return {};
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId==zenId)
      return obj->pose;
  return {};
  }

std::shared_ptr<const DynamicWorld::MovableVobPose> DynamicWorld::rayMovableVob(const Tempest::Vec3& from,
                                                                           const Tempest::Vec3& to) const {
  const auto hit = world->rayCast(from,to,visualMask,false);
  if(!hit.hasHit || hit.body==nullptr || hit.body->ptrType()!=P_MovableVob)
    return {};
  return static_cast<BodyData*>(hit.body->payload())->pose;
  }

size_t DynamicWorld::meleeMovableVobs(const Tempest::Vec3& origin,const Tempest::Vec3& forward,
                                     float rangeCm,float weaponDamage,float strength,FractureRule::Melee kind,
                                     Npc* attacker) {
  if(!movableVobs->destructionEnabled || !std::isfinite(rangeCm) || rangeCm<=0.f ||
     !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
     !std::isfinite(forward.x) || !std::isfinite(forward.z)) return 0;
  const auto facing=Tempest::Vec3(forward.x,0,forward.z);
  if(facing.quadLength()<1.e-6f) return 0;
  const float heading=std::atan2(facing.z,facing.x);
  const int arc=kind==FractureRule::Melee::Fist ? 20 : 60;
  std::unordered_set<BodyData*> touched;
  size_t count=0;
  // Test the swept striking arc against the source collision meshes, including
  // downstrokes at low crates. Every ray stops at its first wall or object;
  // range and facing alone never count as a contact. NPC damage keeps its own
  // existing target/defence calculation and consumes exactly the same RNG.
  for(int yaw=-arc;yaw<=arc;yaw+=10) for(int pitch=-40;pitch<=20;pitch+=20) {
    const float a=heading+float(yaw)*(float(M_PI)/180.f);
    const float p=float(pitch)*(float(M_PI)/180.f);
    const Tempest::Vec3 direction(std::cos(a)*std::cos(p),std::sin(p),std::sin(a)*std::cos(p));
    const auto hit=world->rayCast(origin,origin+direction*rangeCm,visualMask,false);
    if(!hit.hasHit || hit.body==nullptr || hit.body->ptrType()!=P_MovableVob) continue;
    auto& obj=*static_cast<BodyData*>(hit.body->payload());
    if(obj.body==nullptr || obj.pose->broken || obj.pendingBreak || obj.fractureRule==nullptr ||
       obj.movableShape->fracture==nullptr || (!movableVobs->enabled && obj.className!="fence") ||
       touched.find(&obj)!=touched.end()) continue;
    const float damage=obj.fractureRule->meleeDamage(kind,weaponDamage,strength);
    if(!std::isfinite(damage) || damage<=0.f) continue;
    touched.insert(&obj);
    ++count;
    const float impulse=damage*.02f;
    // Fences keep a static sensor hull, so their rejected physical impulse
    // must still record the actor of the real weapon contact.
    obj.body->setTraceCulprit(attacker);
    obj.body->addImpulseAtPoint(direction*impulse,hit.point,attacker);
    obj.health=std::max(0.f,obj.health-damage);
    if(obj.health>.001f) continue;
    obj.pendingBreak=true;
    obj.traceTime=traceTime;
    obj.traceCulprit=obj.body->traceCulprit();
    obj.breakImpulse=impulse;
    obj.breakPoint=hit.point;
    }
  return count;
  }

void DynamicWorld::saveMovableVobs(Serialize& fout) const {
  saveDestructionVobs(fout);
  if(!movableVobs->enabled)
    return;
  std::vector<const BodyData*> moved;
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId!=uint32_t(-1) && obj->body!=nullptr &&
       !PhysicsSave::sameTransform(obj->origin,obj->body->matrix()))
      moved.push_back(obj);
  if(moved.empty())
    return;
  fout.setEntry("worlds/",fout.worldName(),"/movable-vobs");
  fout.write(uint32_t(0x31564f4d),uint16_t(2),fout.worldName(),uint32_t(moved.size()));
  for(const auto* obj:moved)
    fout.write(obj->pose->zenId,obj->visual,obj->origin,obj->body->matrix(),obj->body->isSleeping(),
               obj->trace,uint64_t(obj->traceTime)+1,obj->traceCulprit);
  }

void DynamicWorld::loadMovableVobs(Serialize& fin) {
  loadDestructionVobs(fin);
  if(!movableVobs->enabled || !fin.setEntry("worlds/",fin.worldName(),"/movable-vobs"))
    return;
  try {
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    std::string worldName;
    fin.read(magic,schema,worldName,count);
    if(magic!=0x31564f4d || (schema!=1 && schema!=2) || worldName!=fin.worldName() || count>100000)
      return;
    for(uint32_t i=0;i<count;++i) {
      uint32_t id=uint32_t(-1);
      std::string visual;
      Tempest::Matrix4x4 origin,matrix;
      bool sleeping=true;
      fin.read(id,visual,origin,matrix,sleeping);
      bool trace=false;
      uint64_t traceTimeEncoded=0;
      std::string traceCulprit;
      if(schema>=2)
        fin.read(trace,traceTimeEncoded,traceCulprit);
      if(!PhysicsSave::validTransform(matrix) || !PhysicsSave::sameScale(matrix,origin))
        continue;
      for(auto* obj:movableVobs->bodies) {
        if(obj->pose->zenId!=id || id==uint32_t(-1) || obj->visual!=visual ||
           !PhysicsSave::sameTransform(obj->origin,origin))
          continue;
        if(obj->body==nullptr) {
          if(!PhysicsSave::sameTransform(origin,matrix))
            Tempest::Log::i("movable vob save pose ignored: ",id," ",visual," static (",obj->disposition,")");
          break;
          }
        obj->body->restoreTransform(matrix,sleeping);
        // Legacy poses have no recorded appearance time. Mark an existing
        // trace before synchronizing so loading cannot invent a new timestamp.
        obj->trace = schema==1 ? noticeableMovableTrace(origin,matrix,obj->tilted) : trace;
        obj->traceTime = traceTimeEncoded>0 && traceTimeEncoded-1<=uint64_t(std::numeric_limits<int64_t>::max())
                           ? int64_t(traceTimeEncoded-1) : -1;
        obj->traceCulprit = std::move(traceCulprit);
        // Force query meshes, render callback and attachment pose to synchronize
        // even when the restored body is asleep and will never take a step.
        obj->wasSleeping = false;
        syncMovableVob(*obj);
        break;
        }
      }
    }
  catch(const std::exception& e) {
    Tempest::Log::e("movable vob load: ",e.what());
    }
  }

void DynamicWorld::saveDestructionVobs(Serialize& fout) const {
  if(!movableVobs->destructionEnabled)
    return;
  std::vector<const BodyData*> changed;
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId!=uint32_t(-1) && obj->fractureRule!=nullptr && obj->movableShape->fracture!=nullptr &&
       (obj->pose->broken || obj->health<obj->fractureRule->health))
      changed.push_back(obj);
  if(changed.empty())
    return;
  // Separate optional archive entry: older saves have no destruction state,
  // and movable-vobs readers keep their existing schema and meaning.
  fout.setEntry("worlds/",fout.worldName(),"/destruction-vobs");
  fout.write(uint32_t(0x31534544),uint16_t(2),fout.worldName(),uint32_t(changed.size()));
  for(const auto* obj:changed) {
    fout.write(obj->pose->zenId,obj->visual,obj->origin,obj->current,obj->health,obj->pose->broken,
               uint32_t(obj->fragments.size()),uint64_t(obj->traceTime)+1,obj->traceCulprit);
    for(size_t i=0;i<obj->fragments.size();++i) {
      const auto& piece=obj->fragments[i];
      fout.write(piece!=nullptr,obj->fragmentPermanent[i],uint64_t(obj->fragmentSleepTime[i])+1);
      if(piece!=nullptr)
        fout.write(piece->matrix(),piece->velocity(),piece->angularVelocity(),piece->isSleeping());
      }
    }
  }

void DynamicWorld::loadDestructionVobs(Serialize& fin) {
  if(!movableVobs->destructionEnabled || !fin.setEntry("worlds/",fin.worldName(),"/destruction-vobs"))
    return;
  try {
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    std::string worldName;
    fin.read(magic,schema,worldName,count);
    if(magic!=0x31534544 || (schema!=1 && schema!=2) || worldName!=fin.worldName() || count>100000)
      return;
    struct Piece {
      bool present=false,permanent=false,sleeping=true;
      uint64_t sleepTime=0;
      Tempest::Matrix4x4 matrix;
      Tempest::Vec3 velocity,omega;
      };
    const auto finiteVelocity=[](const Tempest::Vec3& v) {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.quadLength()<1.e14f;
      };
    for(uint32_t record=0;record<count;++record) {
      uint32_t id=uint32_t(-1),pieceCount=0;
      std::string visual;
      Tempest::Matrix4x4 origin,matrix;
      float health=0;
      bool broken=false;
      fin.read(id,visual,origin,matrix,health,broken,pieceCount);
      uint64_t traceTimeEncoded=0;
      std::string traceCulprit;
      if(schema>=2)
        fin.read(traceTimeEncoded,traceCulprit);
      if(pieceCount>20)
        return;
      std::vector<Piece> pieces(pieceCount);
      bool valid=std::isfinite(health) && health>=0.f && PhysicsSave::validTransform(matrix) &&
                 PhysicsSave::sameScale(matrix,origin) && (broken || pieceCount==0);
      size_t permanentCount=0;
      for(auto& piece:pieces) {
        fin.read(piece.present,piece.permanent,piece.sleepTime);
        permanentCount+=piece.permanent;
        valid &= piece.sleepTime<=uint64_t(std::numeric_limits<int64_t>::max())+1;
        valid &= !piece.permanent || piece.present;
        if(piece.present) {
          fin.read(piece.matrix,piece.velocity,piece.omega,piece.sleeping);
          valid &= PhysicsSave::validTransform(piece.matrix) && PhysicsSave::sameScale(piece.matrix,origin) &&
                   finiteVelocity(piece.velocity) && finiteVelocity(piece.omega);
          }
        }
      if(!valid || (broken && permanentCount!=std::min(size_t(3),pieces.size())))
        continue;
      for(auto* obj:movableVobs->bodies) {
        if(id==uint32_t(-1) || obj->pose->zenId!=id || obj->visual!=visual ||
           !PhysicsSave::sameTransform(obj->origin,origin) || obj->fractureRule==nullptr ||
           obj->movableShape->fracture==nullptr || health>obj->fractureRule->health)
          continue;
        if(!broken) {
          obj->health=health;
          break;
          }
        const auto& mesh=*obj->movableShape->fracture;
        if(pieces.size()!=mesh.pieces.size())
          break;
        // Loading debris does not enable movement of any intact prop. Its
        // authored hull is needed only to recover the original fragment mass.
        prepareMovableVobShape(*obj);
        if(!std::isfinite(obj->movableMass) || obj->movableMass<=0.f)
          break;
        std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments(pieces.size());
        for(size_t i=0;i<pieces.size();++i) {
          const auto& piece=pieces[i];
          if(!piece.present)
            continue;
          const float mass=float(obj->movableMass*mesh.pieces[i].volume/mesh.volume);
          auto body=world->addDynamicBody(mesh.pieces[i].hull,piece.matrix,obj->movableShape->friction,mass,C_Fragment);
          body->setLabel(obj->visual+" fragment");
          body->setNoiseProfile(impactMaterial(obj->movableShape->material),obj->movableShape->material,obj->visual+" fragment");
          body->setBuoyancyFraction(vobBuoyancyFraction(obj->movableShape->material));
          body->restoreTransform(piece.matrix,piece.sleeping);
          if(!piece.sleeping)
            body->setVelocity(piece.velocity,piece.omega);
          fragments[i]=std::move(body);
          }
        obj->body.reset();
        obj->query.clear();
        obj->current=matrix;
        obj->pose->transform=matrix;
        obj->pose->broken=true;
        obj->health=0.f;
        obj->pendingBreak=false;
        obj->trace=obj->tilted=false;
        // Step-2 saves have debris but no recorded break time or actor.
        obj->traceTime=traceTimeEncoded>0 && traceTimeEncoded-1<=uint64_t(std::numeric_limits<int64_t>::max())
                         ? int64_t(traceTimeEncoded-1) : -1;
        obj->traceCulprit=std::move(traceCulprit);
        obj->fragments=std::move(fragments);
        obj->fragmentSleepTime.resize(pieces.size());
        obj->fragmentPermanent.resize(pieces.size());
        for(size_t i=0;i<pieces.size();++i) {
          obj->fragmentSleepTime[i]=pieces[i].sleepTime==0 ? -1 : int64_t(pieces[i].sleepTime-1);
          obj->fragmentPermanent[i]=pieces[i].permanent;
          }
        obj->fragmentsWereSleeping=false;
        syncFragments(*obj);
        break;
        }
      }
    }
  catch(const std::exception& e) {
    Tempest::Log::e("destruction vob load: ",e.what());
    }
  }

void DynamicWorld::setTraceTime(int64_t minutes) {
  traceTime = std::max(int64_t(-1),minutes);
  }

void DynamicWorld::setFragmentObserver(const Tempest::Vec3& position) {
  clothWorld->setObserver(position);
  fragmentObserver=position;
  hasFragmentObserver=true;
  }

bool DynamicWorld::setMovableVobTransform(uint32_t zenId, const Tempest::Matrix4x4& matrix, Npc* culprit) {
  if(!movableVobs->enabled || zenId==uint32_t(-1) || !PhysicsSave::validTransform(matrix))
    return false;
  for(auto* obj:movableVobs->bodies) {
    if(obj->pose->zenId!=zenId || obj->body==nullptr || !PhysicsSave::sameScale(matrix,obj->origin))
      continue;
    obj->body->setTraceCulprit(culprit);
    obj->body->restoreTransform(matrix,true);
    obj->wasSleeping = false;
    syncMovableVob(*obj);
    return true;
    }
  return false;
  }

std::vector<DynamicWorld::MovableVobContacts> DynamicWorld::movableVobContacts(float marginCm) const {
  if(marginCm==1.f && !movableVobs->census.empty()) {
    auto result = movableVobs->census;
    for(auto& entry:result)
      for(const auto* obj:movableVobs->bodies)
        if(obj->pose->zenId==entry.zenId) {
          entry.enabled = obj->body!=nullptr;
          break;
          }
    return result;
    }
  static const char* materials[] = {"undef","metal","stone","wood","earth","water","snow","none"};
  constexpr uint32_t mask = (1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_MovableVob);
  std::vector<MovableVobContacts> result;
  for(const auto* obj:movableVobs->bodies) {
    if(obj->body==nullptr || obj->className=="fence")
      continue;
    MovableVobContacts entry;
    entry.zenId = obj->pose->zenId;
    entry.visual = obj->visual;
    entry.className = obj->className;
    entry.disposition = obj->disposition;
    entry.enabled = true;
    entry.mass = obj->movableMass;
    entry.position = obj->body->position();
    JPH::Vec3 min, max;
    obj->body->getAabb(min,max);
    entry.hullMin = CollisionWorld::toCentimeters(min);
    entry.hullMax = CollisionWorld::toCentimeters(max);
    // One entry per body and side: the world mesh is a single body holding both floor and walls.
    using Key = std::pair<const CollisionWorld::CollisionBody*,int>;
    std::vector<Key> order;
    std::map<Key,MovableVobContact> byBody;
    const float height = std::max(entry.hullMax.y-entry.hullMin.y,1.f);
    for(const auto& hit:world->overlaps(*obj->body,marginCm,mask)) {
      const bool below = hit.axis.y<=-0.6f && hit.point1.y<=entry.hullMin.y+0.3f*height;
      // side: 0 below, 1 above, 2..5 = +x,-x,+z,-z by the dominant horizontal component
      int side = below ? 0 : hit.axis.y>=0.6f ? 1 :
                 std::abs(hit.axis.x)>=std::abs(hit.axis.z) ? (hit.axis.x>=0 ? 2 : 3) : (hit.axis.z>=0 ? 4 : 5);
      const Key key{hit.body,side};
      auto [it,inserted] = byBody.try_emplace(key);
      auto& c = it->second;
      if(inserted) {
        order.push_back(key);
        c.below = below;
        c.side = side;
        c.category = hit.category;
        c.depth = hit.depth;
        c.pointYMin = c.pointYMax = hit.point1.y;
        if(hit.body!=nullptr) {
          c.interactive = hit.body->ptrType()==P_Interactive;
          c.other = hit.body->label();
          if(hit.body->ptrType()==P_MovableVob) {
            c.other = static_cast<const BodyData*>(hit.body->payload())->visual;
            c.otherZenId = static_cast<const BodyData*>(hit.body->payload())->pose->zenId;
            }
          hit.body->getAabb(min,max);
          c.otherMin = CollisionWorld::toCentimeters(min);
          c.otherMax = CollisionWorld::toCentimeters(max);
          }
        }
      c.count++;
      c.pointYMin = std::min(c.pointYMin,hit.point1.y);
      c.pointYMax = std::max(c.pointYMax,hit.point1.y);
      if(inserted || hit.depth>c.depth) {
        c.depth = hit.depth;
        c.axis = hit.axis;
        c.point = hit.point1;
        c.material = materials[std::min<size_t>(size_t(hit.material),7)];
        c.sector = hit.sector==nullptr ? "" : hit.sector;
        }
      }
    for(const auto& key:order)
      entry.contacts.push_back(byBody[key]);
    result.push_back(std::move(entry));
    }
  return result;
  }

size_t DynamicWorld::wakeMovableVobs() {
  size_t count = 0;
  for(auto* obj:movableVobs->bodies)
    if(obj->body!=nullptr) {
      obj->body->restoreTransform(obj->body->matrix(),false);
      ++count;
      }
  return count;
  }

std::vector<DynamicWorld::MovableVobSnapshot> DynamicWorld::movableVobSnapshot() const {
  std::vector<MovableVobSnapshot> result;
  result.reserve(movableVobs->bodies.size());
  for(auto* obj:movableVobs->bodies) {
    MovableVobSnapshot entry;
    entry.id = reinterpret_cast<uintptr_t>(obj);
    entry.zenId = obj->pose->zenId;
    entry.origin = obj->origin;
    entry.visual = obj->visual;
    entry.className = obj->className;
    entry.blocksWalking = obj->blocksWalking;
    entry.disposition = obj->disposition;
    entry.legacyCollision = obj->legacyCollision;
    entry.enabled = obj->body!=nullptr;
    entry.mass = obj->movableMass;
    entry.hullVolume = obj->movableShape->hull!=nullptr ?
      float(obj->movableShape->hull->GetVolume()*volumeScale(obj->origin)) : 0.f;
    entry.material = obj->movableShape->material;
    entry.density = materialDensity(entry.material);
    entry.massMultiplier = obj->massMultiplier;
    entry.destructible=obj->fractureRule!=nullptr && obj->movableShape->fracture!=nullptr;
    entry.broken=obj->pose->broken;
    entry.health=obj->health;
    entry.maxHealth=obj->fractureRule!=nullptr ? obj->fractureRule->health : 0.f;
    entry.impactThreshold=obj->fractureRule!=nullptr ? obj->fractureRule->threshold*obj->movableMass : 0.f;
    entry.transform = obj->body!=nullptr ? obj->body->matrix() : obj->current;
    entry.localBounds[0] = obj->localBounds[0];
    entry.localBounds[1] = obj->localBounds[1];
    entry.trace = obj->trace;
    entry.tilted = obj->tilted;
    entry.traceTime = obj->traceTime;
    entry.traceCulprit = obj->traceCulprit;
    if(obj->body!=nullptr) {
      entry.velocity = obj->body->velocity();
      entry.sleeping = obj->body->isSleeping();
      entry.touchedByNpc = obj->body->touchedByNpc();
      }
    result.push_back(std::move(entry));
    }
  return result;
  }

std::vector<DynamicWorld::FragmentSnapshot> DynamicWorld::fragmentSnapshot() const {
  std::vector<FragmentSnapshot> result;
  for(const auto* obj:movableVobs->bodies)
    for(size_t i=0;i<obj->fragments.size();++i) {
      if(obj->fragments[i]==nullptr)
        continue;
      const auto& body=*obj->fragments[i];
      result.push_back({reinterpret_cast<uintptr_t>(obj),obj->pose->zenId,i,body.isSleeping(),
                        body.matrix(),body.velocity(),body.angularVelocity(),obj->fragmentPermanent[i],
                        obj->fragmentSleepTime[i]});
      }
  return result;
  }

std::vector<DynamicWorld::WaterBodySnapshot> DynamicWorld::waterBodySnapshot() const {
  std::vector<WaterBodySnapshot> result;
  std::unordered_map<const CollisionWorld::CollisionBody*,std::pair<const BodyData*,size_t>> identity;
  for(const auto* obj:movableVobs->bodies) {
    if(obj->body!=nullptr) identity.emplace(obj->body.get(),std::make_pair(obj,size_t(-1)));
    for(size_t i=0;i<obj->fragments.size();++i)
      if(obj->fragments[i]!=nullptr) identity.emplace(obj->fragments[i].get(),std::make_pair(obj,i));
    }
  for(const auto& native:world->waterBodySnapshot()) {
    WaterBodySnapshot entry;
    entry.id=native.nativeId;
    entry.ragdoll=native.ragdoll;
    entry.bone=native.bone;
    entry.targetFraction=native.targetFraction;
    entry.category=native.ragdoll!=nullptr ? C_Ragdoll : native.body->category();
    entry.visual=native.body!=nullptr ? native.body->label() : "ragdoll";
    if(native.ragdoll!=nullptr) {
      entry.parentId=reinterpret_cast<uintptr_t>(native.ragdoll);
      entry.piece=native.bone;
      }
    entry.transform=native.transform;
    entry.worldBounds[0]=native.worldBounds[0];
    entry.worldBounds[1]=native.worldBounds[1];
    entry.collisionBounds[0]=native.collisionBounds[0];
    entry.collisionBounds[1]=native.collisionBounds[1];
    if(native.body!=nullptr && native.body->ptrType()==P_Item)
      entry.item=static_cast<const ::Item*>(native.body->payload());
    entry.velocity=native.velocity;
    entry.mass=native.mass;
    entry.density=native.density;
    entry.waterY=native.waterY;
    entry.submergedFraction=native.submergedFraction;
    entry.sleeping=native.sleeping;
    entry.inWater=native.inWater;
    if(const auto found=identity.find(native.body);found!=identity.end()) {
      entry.zenId=found->second.first->pose->zenId;
      entry.parentId=reinterpret_cast<uintptr_t>(found->second.first);
      entry.piece=found->second.second;
      }
    result.push_back(std::move(entry));
    }
  return result;
  }

std::vector<DynamicWorld::RagdollSupport> DynamicWorld::ragdollSupports(const Ragdoll& ragdoll,float marginCm) const {
  std::vector<RagdollSupport> result;
  for(const auto& contact:world->ragdollContacts(ragdoll,marginCm)) {
    RagdollSupport entry;
    entry.bone=contact.bone;
    entry.partBodyId=contact.partBodyId;
    entry.supportBodyId=contact.supportBodyId;
    entry.category=contact.category;
    entry.sleeping=contact.sleeping;
    entry.depth=contact.depth;
    entry.axis=contact.axis;
    entry.point1=contact.point1;
    entry.point2=contact.point2;
    entry.visibleDistance=contact.sourceDistance;
    entry.visiblePoint=contact.sourcePoint;
    entry.sourceTriangle=contact.sourceTriangle;
    entry.kind=contact.category==C_Landscape ? "landscape" : "body";
    if(contact.body!=nullptr)
      entry.visual=contact.body->label();
    const auto queryPoint=CollisionWorld::toMeters(contact.point1);
    float closestSquared=std::numeric_limits<float>::infinity();
    const auto triangle=[&](const Tempest::Vec3 (&vertices)[3],const Tempest::Matrix4x4& matrix) {
      JPH::Vec3 points[3];
      for(size_t i=0;i<3;++i) {
        auto point=vertices[i];
        matrix.project(point);
        points[i]=CollisionWorld::toMeters(point)-queryPoint;
        }
      JPH::uint32 feature=0;
      const auto nearest=JPH::ClosestPoint::GetClosestPointOnTriangle(points[0],points[1],points[2],feature);
      const float distance=nearest.LengthSq();
      if(distance<closestSquared) {
        closestSquared=distance;
        entry.visiblePoint=CollisionWorld::toCentimeters(nearest+queryPoint);
        }
      };
    for(const auto* obj:movableVobs->bodies) {
      size_t piece=size_t(-1);
      bool matches=obj->body.get()==contact.body;
      std::string kind=matches ? "hull" : "query";
      if(!matches)
        for(const auto& query:obj->query)
          matches|=query.get()==contact.body;
      if(!matches)
        for(size_t i=0;i<obj->fragments.size();++i)
          if(obj->fragments[i].get()==contact.body && contact.body!=nullptr) {
            matches=true;
            piece=i;
            kind="fragment";
            break;
            }
      if(!matches || contact.body==nullptr)
        continue;
      entry.parentId=reinterpret_cast<uintptr_t>(obj);
      entry.zenId=obj->pose->zenId;
      entry.visual=obj->visual;
      entry.kind=std::move(kind);
      entry.piece=piece;
      entry.broken=obj->pose->broken;
      entry.visible=piece!=size_t(-1) || !entry.broken;
      if(piece!=size_t(-1)) {
        const auto matrix=obj->fragments[piece]->matrix();
        for(const auto& face:obj->movableShape->fracture->pieces[piece].triangles) {
          const Tempest::Vec3 vertices[]={face.vertices[0].position,face.vertices[1].position,face.vertices[2].position};
          triangle(vertices,matrix);
          }
        }
      else if(entry.visible) {
        for(const auto& part:obj->movableShape->parts) {
          auto matrix=obj->current;
          matrix.mul(part.local);
          for(size_t t=0;t<part.source->mesh.triangleCount();++t) {
            JPH::Vec3 points[3];
            if(!part.source->mesh.triangleVertices(uint32_t(t),points))
              continue;
            const Tempest::Vec3 vertices[]={CollisionWorld::toCentimeters(points[0]),
                                           CollisionWorld::toCentimeters(points[1]),
                                           CollisionWorld::toCentimeters(points[2])};
            triangle(vertices,matrix);
            }
          }
        }
      break;
      }
    if(std::isfinite(closestSquared))
      entry.visibleDistance=std::sqrt(closestSquared)*100.f;
    result.push_back(std::move(entry));
    }
  return result;
  }

void DynamicWorld::damageMovableVob(BodyData& obj,float impulse,const Tempest::Vec3& point,bool arrow,
                                  std::string_view impactCulprit) {
  if(!movableVobs->destructionEnabled || obj.fractureRule==nullptr || obj.movableShape->fracture==nullptr ||
     obj.body==nullptr || obj.pose->broken || obj.pendingBreak || !std::isfinite(impulse) || impulse<=0.f)
    return;
  const auto& rule=*obj.fractureRule;
  const float damage=arrow ? (impulse>=.5f ? impulse*rule.arrowDamage : 0.f) :
                            std::max(impulse/obj.movableMass-rule.threshold,0.f)*rule.impactDamage;
  if(damage<=0.f) return;
  obj.health=std::max(0.f,obj.health-damage);
  if(obj.health>.001f) return;
  obj.pendingBreak=true;
  obj.traceTime=traceTime;
  obj.traceCulprit=impactCulprit.empty() ? obj.body->traceCulprit() : impactCulprit;
  obj.breakImpulse=impulse;
  obj.breakPoint=point;
  }

void DynamicWorld::breakMovableVob(BodyData& obj) {
  if(!obj.pendingBreak || obj.body==nullptr) return;
  const auto started=std::chrono::steady_clock::now();
  const auto matrix=obj.body->matrix();
  const auto velocity=obj.body->velocity(),omega=obj.body->angularVelocity();
  const auto center=obj.body->centerOfMass();
  const auto& mesh=*obj.movableShape->fracture;
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
  fragments.reserve(mesh.pieces.size());
  // All hulls, triangle data and GPU meshes are prepared while loading the
  // visual. The replacement only installs native bodies and transfers motion.
  for(const auto& piece:mesh.pieces) {
    const float mass=float(obj.movableMass*piece.volume/mesh.volume);
    auto body=world->addDynamicBody(piece.hull,matrix,obj.movableShape->friction,mass,C_Fragment);
    body->setLabel(obj.visual+" fragment");
    body->setNoiseProfile(impactMaterial(obj.movableShape->material),obj.movableShape->material,obj.visual+" fragment");
    body->setBuoyancyFraction(vobBuoyancyFraction(obj.movableShape->material));
    const auto r=(body->centerOfMass()-center)*.01f;
    const Tempest::Vec3 tangential(omega.y*r.z-omega.z*r.y,omega.z*r.x-omega.x*r.z,omega.x*r.y-omega.y*r.x);
    // A small separation velocity releases touching cut faces. Linear and
    // angular momentum inherited from the intact body remain the base motion.
    const auto outward=r.quadLength()>1.e-6f ? Tempest::Vec3::normalize(r)*40.f : Tempest::Vec3();
    body->setVelocity(velocity+tangential*100.f+outward,omega);
    fragments.push_back(std::move(body));
    }
  obj.current=matrix;
  obj.pose->transform=matrix;
  obj.pose->broken=true; // Existing pinned arrows invalidate this parent.
  obj.body.reset();
  obj.query.clear();
  obj.fragments=std::move(fragments);
  obj.fragmentSleepTime.assign(obj.fragments.size(),-1);
  obj.fragmentPermanent.assign(obj.fragments.size(),false);
  // Keep stable piece indices throughout cleanup and save/load. Equal-volume
  // pieces are ordered by their authored cache index, making the choice fixed.
  std::vector<size_t> largest(obj.fragments.size());
  for(size_t i=0;i<largest.size();++i) largest[i]=i;
  std::stable_sort(largest.begin(),largest.end(),[&](size_t a,size_t b) {
    return mesh.pieces[a].volume>mesh.pieces[b].volume;
    });
  for(size_t i=0;i<std::min(size_t(3),largest.size());++i)
    obj.fragmentPermanent[largest[i]]=true;
  obj.pendingBreak=false;
  obj.trace=obj.tilted=false;
  obj.fragmentsWereSleeping=false;
  syncFragments(obj);
  PhysicsNoise noise;
  noise.position=center;
  // Some wooden Gothic barrels label their source mesh as stone. Keep that
  // authored density/friction for physics, but use the destructible class for
  // the sound of the newly exposed material.
  const bool wooden=obj.fractureRule->className=="crate" || obj.fractureRule->className=="barrel" ||
                    obj.fractureRule->className=="fence";
  noise.material=wooden ? MAT_WOOD : MAT_CLAY;
  noise.surface=wooden ? asset::MaterialGroup::WOOD : asset::MaterialGroup::STONE;
  noise.what=obj.visual+" break";
  noise.impulse=obj.breakImpulse;
  noise.volume=1.f;
  noise.radius=1600.f;
  noise.body=reinterpret_cast<uintptr_t>(&obj);
  world->emitPhysicsNoise(noise);
  if(TestBench::isActive())
    TestBench::destructionBreak(reinterpret_cast<uintptr_t>(&obj),obj.fragments.size(),
                               float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()));
  }

void DynamicWorld::syncFragments(BodyData& obj) {
  if(obj.fragments.empty()) return;
  const bool sleeping=std::all_of(obj.fragments.begin(),obj.fragments.end(),[](const auto& body) {
    return body==nullptr || body->isSleeping();
    });
  if(sleeping && obj.fragmentsWereSleeping) return;
  obj.fragmentsWereSleeping=sleeping;
  if(!obj.fractureCallback) return;
  std::vector<Tempest::Matrix4x4> matrices;
  matrices.reserve(obj.fragments.size());
  for(const auto& body:obj.fragments) {
    if(body!=nullptr) {
      matrices.push_back(body->matrix());
      }
    else {
      // Preserve the mesh batch-to-piece mapping while hiding retired pieces.
      auto hidden=Tempest::Matrix4x4::mkIdentity();
      hidden.translate(0,-10000000.f,0);
      matrices.push_back(hidden);
      }
    }
  obj.fractureCallback(true,matrices);
  }

void DynamicWorld::cleanupFragments(BodyData& obj) {
  bool changed=false;
  for(size_t i=0;i<obj.fragments.size();++i) {
    auto& body=obj.fragments[i];
    if(body==nullptr)
      continue;
    auto& since=obj.fragmentSleepTime[i];
    if(!body->isSleeping()) {
      since=-1;
      continue;
      }
    if(traceTime>=0 && (since<0 || traceTime<since))
      since=traceTime;
    const bool expired=since>=0 && traceTime>=since && traceTime-since>=30;
    const bool distant=hasFragmentObserver && (body->centerOfMass()-fragmentObserver).quadLength()>3000.f*3000.f;
    if(!obj.fragmentPermanent[i] && (expired || distant)) {
      body.reset();
      changed=true;
      }
    }
  if(changed)
    obj.fragmentsWereSleeping=false;
  }

size_t DynamicWorld::applyMovableVobImpulse(const Tempest::Vec3& center, float radius,
                                           const std::function<Tempest::Vec3(const Tempest::Vec3&,float)>& impulse,
                                           Npc* culprit, const Tempest::Vec3* pointCm) {
  if(!std::isfinite(radius) || radius<=0.f || !impulse)
    return 0;
  size_t count = 0;
  for(auto* obj:movableVobs->bodies) {
    if(obj->body==nullptr || (!movableVobs->enabled && obj->className!="fence"))
      continue;
    const auto target = obj->body->centerOfMass();
    if((target-center).quadLength()>radius*radius)
      continue;
    const auto force = impulse(target,obj->movableMass);
    if(force.quadLength()==0.f)
      continue;
    std::vector<const CollisionWorld::CollisionBody*> excluded;
    excluded.reserve(obj->query.size());
    for(const auto& part:obj->query)
      excluded.push_back(part.get());
    if(world->rayCast(center,target,visualMask,false,nullptr,&excluded).hasHit)
      continue;
    if(obj->className=="fence")
      obj->body->setTraceCulprit(culprit);
    if(obj->className=="fence" || obj->body->addImpulse(force,culprit,pointCm)) {
      ++count;
      damageMovableVob(*obj,force.length(),pointCm!=nullptr ? *pointCm : target);
      }
    }
  return count;
  }

DynamicWorld::BulletBody* DynamicWorld::bulletObj(BulletCallback* cb) {
  return bulletList->add(cb);
  }

DynamicWorld::BBoxBody DynamicWorld::bboxObj(BBoxCallback* cb, const zenkit::AxisAlignedBoundingBox& bbox) {
  return BBoxBody(this,cb,bbox);
  }

DynamicWorld::BBoxBody DynamicWorld::bboxObj(BBoxCallback* cb, const Tempest::Vec3& pos, float R) {
  return BBoxBody(this,cb,pos,R);
  }

void DynamicWorld::moveBullet(BulletBody &b, const Tempest::Vec3& dir, uint64_t dt) {
  ArrowTrace::write(b,b.cb,"begin",dt);
  const float dtF     = float(dt);
  const bool  isSpell = b.isSpell();

  auto  pos = b.pos;
  auto  to  = pos + dir*dtF - Tempest::Vec3(0,(isSpell ? 0 : 0.5f*gravity*dtF*dtF),0);

  if(auto ptr = bboxList->rayTest(pos,to)) {
    if(ptr->cb!=nullptr)
      ptr->cb->onCollide(b);
    }
  const bool attachAtWorldStop=!isSpell && b.cb!=nullptr && b.cb->stopsAtWorldHit();
  const auto hit = world->rayCast(pos,to,visualMask,false);
  const auto matId = hit.hasHit ? hit.material : asset::MaterialGroup::NONE;
  ArrowTrace::write(b,b.cb,"world_query",dt,nullptr,hit.hasHit,hit.fraction,hit.point,hit.normal,
                    unsigned(matId),hit.body!=nullptr ? unsigned(hit.body->category()) : 0);

  // Cloth has no blocking collision shape. Notify only the travelled part of
  // this step: either a solid world contact or an accepted NPC contact ends it.
  const bool hasCloth=clothWorld->enabled() && clothWorld->bodyCount()!=0;
  const auto notifyCloth=[&](const Tempest::Vec3& end) {
    if(!hasCloth)
      return;
    const auto incoming=to-pos;
    if(incoming.quadLength()<=1.e-8f)
      return;
    if(!isSpell) {
      clothWorld->projectile(pos,end,Tempest::Vec3::normalize(incoming)*.35f,true);
      }
    else if(ownerWorld!=nullptr) {
      const auto kind=ownerWorld->script().physicalSpell(b.spellId());
      if(kind==GameScript::PhysicalSpell::Fireball || kind==GameScript::PhysicalSpell::WindFist)
        clothWorld->projectile(pos,end,Tempest::Vec3::normalize(incoming)*(kind==GameScript::PhysicalSpell::WindFist ? 8.f : 5.f),false);
      }
    };

  // an arrow aimed by hand: the npcs on the way to the wall first (one stands right before it), hit
  // where their bones are
  const bool precise = !isSpell && b.cb!=nullptr && b.cb->isPrecise();
  if(precise) {
    const float a   = matId!=asset::MaterialGroup::NONE ? hit.fraction : 1.f;
    const auto  end = pos + (to-pos)*a;
    if(npcList->rayTestAll(pos,end,b.targetRange(),[&](Npc& npc) {
      // Capture the same bone intersection before damage can alter its pose.
      float distance=0;
      const bool boneHit=hasCloth && npc.rayHitZone(pos,end,distance)!=HitZones::None;
      if(!b.cb->onCollidePrecise(npc,pos,end))
        return false;
      const auto impact=boneHit ? pos+Tempest::Vec3::normalize(end-pos)*distance : end;
      notifyCloth(impact);
      return true;
      })) {
      b.cb->onStop();
      ArrowTrace::write(b,b.cb,"end",dt);
      return;
      }
    }

  bool stopBullet = false;
  if(matId != asset::MaterialGroup::NONE) {
    notifyCloth(pos+(to-pos)*hit.fraction);
    if(!isSpell && hit.body!=nullptr && hit.body->ptrType()==P_MovableVob) {
      auto& target=*static_cast<BodyData*>(hit.body->payload());
      // Direction is cm/ms; a 120 gram arrow transfers SI momentum at its
      // actual flight speed, independently of optional arrow attachments.
      if(movableVobs->destructionEnabled && target.movableShape->fracture!=nullptr) {
        const auto impulse=b.direction()*1.2f;
        if(target.body!=nullptr) {
          auto* culprit=b.cb!=nullptr ? b.cb->origin() : nullptr;
          target.body->setTraceCulprit(culprit);
          target.body->addImpulseAtPoint(impulse,hit.point,culprit);
          }
        damageMovableVob(target,impulse.length(),hit.point,true);
        }
      }
    if(isSpell){
      if(b.cb!=nullptr) {
        BulletWorldHit result;
        result.point=hit.point;
        result.normal=hit.normal;
        result.incoming=to-pos;
        result.material=matId;
        result.ragdoll=hit.ragdoll;
        result.bone=hit.bone;
        if(hit.body!=nullptr && hit.body->ptrType()==P_MovableVob)
          result.movable=static_cast<BodyData*>(hit.body->payload())->pose;
        if(hit.body!=nullptr && hit.body->category()==C_Object && hit.body->ptrType()==P_Interactive)
          result.vob=reinterpret_cast<Interactive*>(hit.body->payload());
        b.cb->onSpellWorldHit(result);
        }
      stopBullet = true;
      } else {
      if(matId==asset::MaterialGroup::METAL ||
         matId==asset::MaterialGroup::STONE) {
        auto m = b.dir;
        auto n = Tempest::Vec3::normalize(hit.normal);
        const float l = b.speed();
        m/=l;

        auto reflected = m - n*(2.f*Tempest::Vec3::dotProduct(m,n));
        reflected*=(l*0.5f); // slow-down

        float a = hit.fraction;
        b.move(pos + (to-pos)*a);
        if(l*a>0.1f) {
          b.setDirection(reflected);
          b.addPathLen((to-pos).length()*a);
          b.addHit();
          if(b.cb!=nullptr)
            b.cb->onCollide(matId);
          }
        } else {
        float a = hit.fraction;
        b.move(pos + (to-pos)*a);
        b.addPathLen((to-pos).length()*a);
        b.addHit();
        if(b.cb!=nullptr)
          b.cb->onCollide(matId);
        stopBullet = true;
        }
      }
    } else {
    auto clothEnd=to;
    float npcDistance=0;
    if(precise)
      ; // tested above
    else if(auto ptr = npcList->rayTest(pos,to,b.targetRange(),nullptr,&npcDistance)) {
      ArrowTrace::write(b,b.cb,"npc_collision",dt,ptr->toNpc());
      if(b.cb!=nullptr)
        stopBullet |= b.cb->onCollide(*ptr->toNpc()); else
        stopBullet  = true;
      if(stopBullet)
        clothEnd=pos+Tempest::Vec3::normalize(to-pos)*npcDistance;
      }
    notifyCloth(clothEnd);
    const float l = b.speed();
    auto        d = b.direction();
    if(!isSpell)
      d.y -= (gravity)*dtF;
    b.move(to);
    b.setDirection(d);
    b.addPathLen(l*dtF);
    }

  if(stopBullet || b.hitCount()>3 || b.pathLength()>10000) {
    ArrowTrace::write(b,b.cb,"stop",dt,nullptr,hit.hasHit,hit.fraction,hit.point,hit.normal,
                      unsigned(matId),hit.body!=nullptr ? unsigned(hit.body->category()) : 0);
    if(b.cb!=nullptr) {
      // Pin only after the unchanged flight code has decided to stop at a solid
      // world contact. In particular, stone/metal still reflect before this test.
      if(attachAtWorldStop && matId!=asset::MaterialGroup::NONE) {
        BulletWorldHit result;
        result.point=b.position();
        result.normal=hit.normal;
        result.incoming=to-pos;
        result.material=matId;
        result.ragdoll=hit.ragdoll;
        result.bone=hit.bone;
        if(hit.body!=nullptr && hit.body->ptrType()==P_MovableVob)
          result.movable=static_cast<BodyData*>(hit.body->payload())->pose;
        if(hit.body!=nullptr && hit.body->category()==C_Object && hit.body->ptrType()==P_Interactive)
          result.vob=reinterpret_cast<Interactive*>(hit.body->payload());
        b.worldStopped=true;
        b.cb->onWorldHit(result);
        }
      b.cb->onStop();
      }
    }
  ArrowTrace::write(b,b.cb,"end",dt);
  }

void DynamicWorld::tick(uint64_t dt) {
  if(ownerWorld!=nullptr) {
    const float cycle=Gothic::settingsGetF("ENGINE","zWindCycleTime")+
                      Gothic::settingsGetF("ENGINE","zWindCycleTimeVar");
    const uint64_t period=std::isfinite(cycle) && cycle>0.f ? uint64_t(cycle*1000.f) : 0;
    clothWorld->setWind(WorldWind::sample(ownerWorld->tickCount(),period,
                                        Gothic::settingsGetI("ENGINE","zWindEnabled")!=0));
    }
  npcList   ->tickAabbs();
  bulletList->tick(dt);
  world     ->tick(dt);
  {
    FrameProfile::Scope timing(FrameProfile::Zone::Destruction);
    for(auto* obj:movableVobs->bodies)
      if(movableVobs->enabled || obj->className=="fence" || obj->pose->broken) {
        if(obj->pendingBreak) breakMovableVob(*obj);
        syncMovableVob(*obj);
        cleanupFragments(*obj);
        syncFragments(*obj);
        }
    }
  // NPC walking runs before native body integration. Finish only the motion
  // of an already accepted moving support against the integrated world.
  // onMove may thaw a frozen entry, so collect pointers before updating them.
  std::vector<Npc*> riders;
  for(const auto* records:{&npcList->body,&npcList->frozen})
    for(const auto& record:*records)
      if(record.body!=nullptr && record.body->virtualCharacter!=nullptr)
        if(auto* npc=record.body->toNpc())
          riders.push_back(npc);
  for(auto* npc:riders)
    npc->finishCharacterVirtualSupport(dt);
  }

std::unique_ptr<Ragdoll> DynamicWorld::createRagdoll(const Skeleton& skeleton, const HitZones& zones,
                                                   const Tempest::Matrix4x4* worldPose, size_t numBones,
                                                   const Tempest::Matrix4x4& objectMatrix,
                                                   const Tempest::Vec3& initialVelocity) {
  return world->createRagdoll(skeleton,zones,worldPose,numBones,objectMatrix,initialVelocity);
  }

std::unique_ptr<Ragdoll> DynamicWorld::createKinematicBody(const Skeleton& skeleton, const HitZones& zones,
                                                        const Tempest::Matrix4x4* worldPose, size_t numBones,
                                                        const Tempest::Matrix4x4& objectMatrix) {
  return world->createKinematicBody(skeleton,zones,worldPose,numBones,objectMatrix);
  }

DynamicWorld::PhysicsStats DynamicWorld::physicsStats() const {
  return {world->stepMilliseconds(),world->bodyCount(),world->rayCount()};
  }

uint64_t DynamicWorld::simulationStep() const { return world->simulationStep(); }
void DynamicWorld::configureMeleeBench(int substeps) { world->configureMeleeBench(substeps); }
void DynamicWorld::updateMeleeBlade(const Npc* owner,const Tempest::Matrix4x4& hand,float lengthCm) { world->updateMeleeBlade(owner,hand,lengthCm); }
void DynamicWorld::removeMeleeBlade(const Npc* owner) { world->removeMeleeBlade(owner); }
void DynamicWorld::updateShadowBody(Npc* owner,const std::vector<MeleeBodyCapsule>& capsules) { world->updateShadowBody(owner,capsules); }
void DynamicWorld::removeShadowBody(const Npc* owner) { world->removeShadowBody(owner); }
void DynamicWorld::updateShadowBlade(const Npc* owner,uint64_t swingId,const std::vector<MeleeBladeSegment>& segments,
                                     uint64_t timeMs,double activeBeginMs,double activeEndMs,float walkableCos,
                                     Tempest::Vec3 forward) {
  world->updateShadowBlade(owner,swingId,segments,timeMs,activeBeginMs,activeEndMs,walkableCos,forward);
  }
std::vector<MeleeBladeContact> DynamicWorld::takeShadowBladeContacts(const Npc* owner) { return world->takeShadowBladeContacts(owner); }
void DynamicWorld::removeShadowBlade(const Npc* owner) { world->removeShadowBlade(owner); }
void DynamicWorld::flushShadowBlades() { world->flushShadowBlades(); }
float DynamicWorld::meleeBladeAdvance(const std::vector<MeleeBladeSegment>& blades, const std::vector<MeleeBodyCapsule>& target,
                                    const Tempest::Vec3& direction, float maxAdvanceCm) const {
  return world->meleeBladeAdvance(blades,target,direction,maxAdvanceCm);
  }
void DynamicWorld::accountMeleeReachTime(float milliseconds) { world->accountMeleeReachTime(milliseconds); }
void DynamicWorld::measureMeleeBench(std::string path) { world->measureMeleeBench(std::move(path)); }
void DynamicWorld::reportMeleeBench() { world->reportMeleeBench(); }

NavMesh::Geometry DynamicWorld::navigationGeometry() const {
  NavMesh::Geometry result;
  world->staticTriangles([&](const CollisionWorld::StaticTriangle& triangle) {
    auto& vertices = triangle.water ? result.waterVertices : result.vertices;
    auto& indices  = triangle.water ? result.waterTriangles : result.triangles;
    if(vertices.size()/3>size_t(std::numeric_limits<int>::max())-3)
      throw std::runtime_error("Static navigation geometry exceeds the index limit");
    const int first = int(vertices.size()/3);
    for(const auto& vertex:triangle.vertices) {
      vertices.push_back(vertex.x);
      vertices.push_back(vertex.y);
      vertices.push_back(vertex.z);
      }
    indices.push_back(first);
    indices.push_back(first+1);
    indices.push_back(first+2);
    });
  return result;
  }

void DynamicWorld::deleteObj(BulletBody* obj) {
  bulletList->del(obj);
  }

float DynamicWorld::materialFriction(asset::MaterialGroup mat) {
  // https://www.thoughtspike.com/friction-coefficients-for-bullet-physics/
  switch(mat) {
    case asset::MaterialGroup::UNDEFINED:
      return 0.5f;
    case asset::MaterialGroup::METAL:
      return 1.1f;
    case asset::MaterialGroup::STONE:
      return 0.65f;
    case asset::MaterialGroup::WOOD:
      return 0.4f;
    case asset::MaterialGroup::EARTH:
      return 0.4f;
    case asset::MaterialGroup::WATER:
      return 0.01f;
    case asset::MaterialGroup::SNOW:
      return 0.2f;
    case asset::MaterialGroup::NONE:
      break;
    }
  return 0.75f;
  }

float DynamicWorld::materialDensity(asset::MaterialGroup mat) {
  switch (mat) {
    case asset::MaterialGroup::UNDEFINED:
    case asset::MaterialGroup::NONE:
      return 2000.0f;
    case asset::MaterialGroup::METAL:
      return 7800.f;
    case asset::MaterialGroup::STONE:
      return 2200.f;
    case asset::MaterialGroup::WOOD:
      return 700.f;
    case asset::MaterialGroup::EARTH:
      return 1500.f;
    case asset::MaterialGroup::WATER:
      return 1000.f;
    case asset::MaterialGroup::SNOW:
      return 1000.f;
    }
  return 2000.f;
  }

float DynamicWorld::rayBox(const Tempest::Vec3& orig, const Tempest::Vec3& dir, const float TMax,
                           const Tempest::Matrix4x4& obj, const Tempest::Vec3& min, const Tempest::Vec3& max,
                           const float padd) {
  using namespace Tempest;

  auto tmp = obj;
  tmp.inverse();

  auto tOri = orig;
  auto tDir = dir;
  auto zero = 0.f;
  tmp.project(tOri);
  tmp.project(tDir.x, tDir.y, tDir.z, zero);

  float tHit = rayBox(tOri, Vec3(tDir.x, tDir.y, tDir.z), TMax, min, max, padd);
  if(tHit==TMax)
    return TMax;

  //NOTE: worry about non-uniform scale matrix
  tDir *= tHit; zero = 0;
  tmp.project(tDir.x, tDir.y, tDir.z, zero);
  return tDir.length();
  }

float DynamicWorld::rayBox(const Tempest::Vec3& orig, const Tempest::Vec3& dir, const float TMax,
                           const Tempest::Vec3& boxMin, const Tempest::Vec3& boxMax,
                           const float padd) {
  using namespace Tempest;

  Vec3  invDir = Vec3(1.f/dir.x, 1.f/dir.y, 1.f/dir.z);

  Vec3  tMin  = (boxMin - orig)*invDir;
  Vec3  tMax  = (boxMax - orig)*invDir;
  Vec3  t1    = Vec3{std::min(tMin.x, tMax.x), std::min(tMin.y, tMax.y), std::min(tMin.z, tMax.z)};
  Vec3  t2    = Vec3{std::max(tMin.x, tMax.x), std::max(tMin.y, tMax.y), std::max(tMin.z, tMax.z)};

  float tNear = std::max(0.f,  std::max(t1.x, std::max(t1.y, t1.z)));
  float tFar  = std::min(TMax, std::min(t2.x, std::min(t2.y, t2.z)));

  return tNear > tFar ? TMax : std::max(0.f, tNear-padd);
  }

std::string_view DynamicWorld::validateSectorName(std::string_view name) const {
  return landMesh->validateSectorName(name);
  }

bool DynamicWorld::hasCollision(const NpcItem& it, CollisionTest& out) {
  bool ret = false;

  out.normal = Tempest::Vec3();
  if(npcList->hasCollision(it,out.normal,out.npc,out.npcCol)){
    ret = true;
    }
  if(it.obj->shape!=nullptr && world->hasCollision(it.obj->shape.GetPtr(),it.obj->centerPosition(),out.normal,out.vob)) {
    out.landCol = true;
    ret = true;
    }

  if(!ret)
    return false;
  out.normal = Tempest::Vec3::normalize(out.normal);
  return true;
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
    bool limitStairAdvance, bool bounceOnSteepContact) {
  return implMoveVirtual(displacement,dt,stepHeight,maxSlope,mass,culprit,false,limitStairAdvance,bounceOnSteepContact);
  }

DynamicWorld::NpcItem::VirtualMoveResult DynamicWorld::NpcItem::carryVirtual(uint64_t dt, Npc* culprit) {
  return implMoveVirtual({},dt,0,0,0,culprit,true);
  }

DynamicWorld::NpcItem::VirtualMoveResult DynamicWorld::NpcItem::implMoveVirtual(
    const Tempest::Vec3& displacement, uint64_t dt, float stepHeight, float maxSlope, float mass, Npc* culprit, bool carry,
    bool limitStairAdvance, bool bounceOnSteepContact) {
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
                       obj->virtualCharacter->move(requested,dt,stepHeight,maxSlope,mass,culprit,limitStairAdvance);
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

DynamicWorld::Item::~Item() {
  delete obj;
  }

void DynamicWorld::Item::setObjMatrix(const Tempest::Matrix4x4& m) {
  if(obj==nullptr)
    return;
  if(obj->movableShape!=nullptr) {
    obj->origin = m;
    owner->rebuildMovableVob(*obj);
    }
  else if(obj->body!=nullptr)
    obj->body->setObjMatrix(m);
  }

void DynamicWorld::Item::setAttachmentMatrix(const Tempest::Matrix4x4& m) {
  if(obj==nullptr || obj->movableShape==nullptr || (obj->body!=nullptr && obj->body->mass()>0.f) || obj->pose->broken)
    return;
  obj->current = m;
  obj->pose->transform = m;
  if(obj->body!=nullptr)
    obj->body->setObjMatrix(m);
  for(size_t i=0;i<obj->query.size();++i) {
    auto matrix = m;
    matrix.mul(obj->movableShape->parts[i].local);
    obj->query[i]->setObjMatrix(matrix);
    }
  }

void DynamicWorld::Item::setItem(::Item* it) {
  assert(obj->body->category()==DynamicWorld::C_Item);
  obj->body->setPayload(it,P_Item);
  if(it!=nullptr) {
    const auto material=ItemMaterial(it->handle().material);
    const auto surface=material==MAT_WOOD ? asset::MaterialGroup::WOOD :
                       material==MAT_METAL ? asset::MaterialGroup::METAL : asset::MaterialGroup::STONE;
    obj->body->setNoiseProfile(material,surface,it->handle().visual);
    obj->body->setLabel(it->handle().visual);
    }
  }

void DynamicWorld::Item::setInteractive(Interactive* it) {
  assert(obj->body->category()==DynamicWorld::C_Object);
  obj->body->setPayload(it,P_Interactive);
  }

void DynamicWorld::Item::setPayloadPtr(void* ptr) {
  if(obj!=nullptr && obj->body!=nullptr)
    obj->body->setPayload(ptr,P_Editor);
  if(obj!=nullptr)
    for(auto& query:obj->query)
      query->setPayload(ptr,P_Editor);
  }

Tempest::Matrix4x4 DynamicWorld::Item::matrix() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->matrix() : Tempest::Matrix4x4::mkIdentity();
  }

Tempest::Vec3 DynamicWorld::Item::position() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->position() : Tempest::Vec3();
  }

void DynamicWorld::Item::restorePhysics(const Tempest::Matrix4x4& matrix, bool sleeping,
                                      const Tempest::Vec3& velocity, const Tempest::Vec3& angular) {
  if(obj==nullptr || obj->body==nullptr) return;
  obj->body->restoreTransform(matrix,sleeping);
  // A nonfinite restore recovered to rest; do not overwrite its zero velocity.
  if(!sleeping && std::isfinite(matrix.at(3,0)) && std::isfinite(matrix.at(3,1)) && std::isfinite(matrix.at(3,2)))
    obj->body->setVelocity(velocity,angular);
  }

float DynamicWorld::Item::mass() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->mass() : 0.f;
  }

Tempest::Vec3 DynamicWorld::Item::velocity() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->velocity() : Tempest::Vec3{};
  }

Tempest::Vec3 DynamicWorld::Item::angularVelocity() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->angularVelocity() : Tempest::Vec3{};
  }

bool DynamicWorld::Item::isSleeping() const {
  return obj==nullptr || obj->body==nullptr || obj->body->isSleeping();
  }

void DynamicWorld::Item::retainForItemWake() {
  if(obj!=nullptr && obj->body!=nullptr)
    obj->body->retainForItemWake();
  }

bool DynamicWorld::Item::touchedByNpc() const {
  return obj!=nullptr && obj->body!=nullptr && obj->body->touchedByNpc();
  }

bool DynamicWorld::Item::addImpulse(const Tempest::Vec3& impulseNs, Npc* culprit) {
  if(obj==nullptr || obj->body==nullptr || !obj->body->addImpulse(impulseNs,culprit)) return false;
  if(obj->movableShape!=nullptr) owner->damageMovableVob(*obj,impulseNs.length(),obj->body->centerOfMass());
  return true;
  }

std::shared_ptr<const FractureMesh> DynamicWorld::Item::fractureMesh() const {
  return obj!=nullptr && obj->movableShape!=nullptr ? obj->movableShape->fracture : nullptr;
  }

bool DynamicWorld::Item::isBroken() const {
  return obj!=nullptr && obj->pose->broken;
  }

void DynamicWorld::Item::setFractureCallback(std::function<void(bool,const std::vector<Tempest::Matrix4x4>&)> callback) {
  if(obj==nullptr) return;
  obj->fractureCallback=std::move(callback);
  if(obj->pose->broken) {
    obj->fragmentsWereSleeping=false;
    owner->syncFragments(*obj);
    }
  else if(obj->fractureCallback) obj->fractureCallback(false,{});
  }

void DynamicWorld::Item::setTransformCallback(std::function<void(const Tempest::Matrix4x4&)> callback) {
  if(obj==nullptr)
    return;
  obj->transformCallback = std::move(callback);
  if(obj->transformCallback && obj->movableShape!=nullptr)
    obj->transformCallback(obj->current);
  }

DynamicWorld::BulletBody::BulletBody(DynamicWorld* wrld, DynamicWorld::BulletCallback* cb)
  :owner(wrld), cb(cb) {
  }

DynamicWorld::BulletBody::BulletBody(DynamicWorld::BulletBody&& other)
  : pos(other.pos), lastPos(other.lastPos),
    dir(other.dir), dirL(other.dirL), totalL(other.totalL), spl(other.spl), worldStopped(other.worldStopped){
  std::swap(owner,other.owner);
  std::swap(cb,other.cb);
  }

void DynamicWorld::BulletBody::setSpellId(int s) {
  spl = s;
  }

void DynamicWorld::BulletBody::setTargetRange(float t) {
  tgRange = t;
  }

void DynamicWorld::BulletBody::move(const Tempest::Vec3& to) {
  lastPos = pos;
  pos     = to;
  }

void DynamicWorld::BulletBody::setPosition(const Tempest::Vec3& p) {
  lastPos = p;
  pos     = p;
  }

void DynamicWorld::BulletBody::setDirection(const Tempest::Vec3& d) {
  dir  = d;
  dirL = d.length();
  }

float DynamicWorld::BulletBody::pathLength() const {
  return totalL;
  }

void DynamicWorld::BulletBody::addPathLen(float v) {
  totalL += v;
  }

Tempest::Matrix4x4 DynamicWorld::BulletBody::matrix() const {
  const float dx = dir.x/dirL;
  const float dy = dir.y/dirL;
  const float dz = dir.z/dirL;

  float a2  = std::asin(dy)*float(180/M_PI);
  float ang = std::atan2(dz,dx)*float(180/M_PI)+180.f;

  Tempest::Matrix4x4 mat;
  mat.identity();
  mat.translate(pos.x,pos.y,pos.z);
  mat.rotateOY(-ang);
  mat.rotateOZ(-a2);
  return mat;
  }

DynamicWorld::BBoxBody::BBoxBody(DynamicWorld* ow, BBoxCallback* cb, const zenkit::AxisAlignedBoundingBox& bbox)
  :owner(ow),cb(cb) {
  const auto extent = Tempest::Vec3(bbox.max.x-bbox.min.x,bbox.max.y-bbox.min.y,bbox.max.z-bbox.min.z)*0.5f;
  const auto center = Tempest::Vec3(bbox.max.x+bbox.min.x,bbox.max.y+bbox.min.y,bbox.max.z+bbox.min.z)*0.5f;
  const auto halfExtent = JPH::Vec3::sMax(CollisionWorld::toMeters(extent),JPH::Vec3::sReplicate(0.0001f));
  JPH::ShapeRefC box = new JPH::BoxShape(halfExtent,0.f);
  Tempest::Matrix4x4 m;
  m.identity();
  m.translate(center);
  auto data = std::make_unique<BBoxShape>();
  data->body = owner->world->addCollisionBody(box,m,0,C_Ghost,false,true);
  shape = data.release();
  owner->bboxList->add(this);
  }

DynamicWorld::BBoxBody::BBoxBody(DynamicWorld* ow, BBoxCallback* cb, const Tempest::Vec3& pos, float R)
  :owner(ow),cb(cb) {
  // This overload historically takes its radius in meters and its cylinder height is four meters.
  JPH::ShapeRefC capsule = new JPH::CapsuleShape(2.f,std::max(R,0.0001f));
  Tempest::Matrix4x4 m;
  m.identity();
  m.translate(pos);
  auto data = std::make_unique<BBoxShape>();
  data->body = owner->world->addCollisionBody(capsule,m,0,C_Ghost,false,true);
  shape = data.release();
  owner->bboxList->add(this);
  }

DynamicWorld::BBoxBody::BBoxBody(BBoxBody&& other)
  :owner(other.owner),cb(other.cb),shape(other.shape) {
  other.owner = nullptr;
  other.cb    = nullptr;
  other.shape = nullptr;

  if(owner!=nullptr) {
    owner->bboxList->del(&other);
    owner->bboxList->add(this);
    }
  }

DynamicWorld::BBoxBody& DynamicWorld::BBoxBody::operator=(BBoxBody&& other) {
  if(other.owner!=nullptr)
    other.owner->bboxList->del(&other);
  if(owner!=nullptr)
    owner->bboxList->del(this);

  std::swap(owner,other.owner);
  std::swap(cb,   other.cb);
  std::swap(shape,other.shape);

  if(other.owner!=nullptr)
    other.owner->bboxList->add(&other);
  if(owner!=nullptr)
    owner->bboxList->add(this);

  return *this;
  }

DynamicWorld::BBoxBody::~BBoxBody() {
  if(owner==nullptr)
    return;
  owner->bboxList->del(this);
  delete shape;
  }
