#pragma once

#include "dynamicworld.h"
#include "collisionworld.h"
#include "convexparts.h"

#include <Tempest/Log>

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <list>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace DynamicWorldInternal {
constexpr uint32_t landscapeMask = (1u << DynamicWorld::C_Landscape) | (1u << DynamicWorld::C_Object) |
                                    (1u << DynamicWorld::C_MovableQuery);
constexpr uint32_t visualMask    = landscapeMask | (1u << DynamicWorld::C_SmallVobQuery);

struct FractureMotion {
  float hit = 150.f, falloff = 60.f, outward = 120.f, spin = 3.f, cap = 200.f;

  static FractureMotion load() {
    FractureMotion motion;
    const auto* env=std::getenv("OPENGOTHIC_FRACTURE_MOTION");
    if(env==nullptr || env[0]=='\0') return motion;
    const auto invalid=[](const std::string& entry, const char* reason) {
      Tempest::Log::e("Invalid OPENGOTHIC_FRACTURE_MOTION entry '",entry,"': ",reason,"; using defaults");
      return FractureMotion{};
      };
    std::istringstream entries(env);
    for(std::string entry;std::getline(entries,entry,',');) {
      std::istringstream field(entry);
      field.imbue(std::locale::classic());
      std::string key;
      float value=0;
      if(!std::getline(field,key,'=') || !(field>>value) || !(field>>std::ws).eof() ||
         !std::isfinite(value) || value<0.f)
        return invalid(entry,"expected key=value with a finite non-negative number");
      const auto first=key.find_first_not_of(" \t\r\n\f\v");
      if(first==std::string::npos) return invalid(entry,"missing key");
      key=key.substr(first,key.find_last_not_of(" \t\r\n\f\v")-first+1);
      if(key=="hit") motion.hit=value;
      else if(key=="falloff") {
        if(value==0.f) return invalid(entry,"falloff must be greater than zero");
        motion.falloff=value;
        }
      else if(key=="outward") motion.outward=value;
      else if(key=="spin") motion.spin=value;
      else if(key=="cap") motion.cap=value;
      else return invalid(entry,"unknown key");
      }
    return motion;
    }
  };

inline ItemMaterial impactMaterial(asset::MaterialGroup material) {
  switch(material) {
    case asset::MaterialGroup::METAL: return MAT_METAL;
    case asset::MaterialGroup::STONE: return MAT_STONE;
    case asset::MaterialGroup::WOOD:  return MAT_WOOD;
    default:                          return MAT_CLAY;
    }
  }

// Water displacement target of a prop's or a piece's wood (classless wood: solid wood).
float vobBuoyancyFraction(asset::MaterialGroup material, std::string_view className = {});
// The pieces of each bond group of one break, in piece order; groups by index.
std::vector<std::vector<uint32_t>> fractureMembers(const std::vector<uint32_t>& pieceGroup);
// The native body of one bond group of a break, in the parent's pose (dynamicworld_fragments.cpp).
std::unique_ptr<CollisionWorld::DynamicBody> addFragmentBody(CollisionWorld& world, const FractureMesh& mesh,
                                                             const std::vector<uint32_t>& members,
                                                             const Tempest::Matrix4x4& matrix, float parentMass,
                                                             asset::MaterialGroup parentMaterial, float parentFriction,
                                                             const std::string& visual, const JPH::GroupFilter* filter,
                                                             uint32_t breakGroup, uint32_t group, void* parent);
// The motion of a new body of a break: the old body's motion at its centre of mass (`center` is the
// old one's), and the blow by point 6 of the bonds note: a single piece also gets the spin and,
// with `outward`, the push from the centre; a joined group only the decaying blow by its own centre
// of mass. A remnant's break has no outward push: its loose staves fall by their own weight.
void launchFragment(CollisionWorld::DynamicBody& fragment, const std::vector<uint32_t>& members, const FractureMotion& motion,
                    const Tempest::Vec3& center, const Tempest::Vec3& velocity, const Tempest::Vec3& omega,
                    const Tempest::Vec3& point, const Tempest::Vec3& direction, size_t version, bool outward = true);

inline double volumeScale(const Tempest::Matrix4x4& matrix) {
  const auto* m = matrix.data();
  return std::abs(double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
                  double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
                  double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2]));
  }

}

using namespace DynamicWorldInternal;

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
    bool                       convexPartsAttempted = false;
    std::shared_ptr<const ConvexParts> convexParts;
    std::shared_ptr<const FractureMesh> fracture;
    };
  std::unordered_map<std::string,std::shared_ptr<Shape>> shapes;
  std::vector<BodyData*> bodies;
  std::vector<MovableVobContacts> census;
  const FractureMotion fractureMotion = FractureMotion::load();
  const FractureBonds fractureBonds = FractureBonds::load();
  double validationMs = 0;
  bool finalized = false;
  bool enabled = true;
  bool furniturePartsEnabled = true;
  bool destructionEnabled = true;
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
  Tempest::Vec3 breakPoint, breakDirection;
  FractureHit breakHit = FractureHit::Impact;
  bool pendingBreak = false, fragmentsWereSleeping = true;
  // One native body per bond group (reports/physics/2026-11-destruction-bonds): fragments,
  // their sleep time and permanence are per group, fragmentGroup maps each piece to its
  // group. Without bonds every piece is its own group, numbered as the piece.
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
  std::vector<int64_t> fragmentSleepTime;
  std::vector<bool> fragmentPermanent;
  std::vector<uint32_t> fragmentGroup;
  // Remnants, groups of several pieces, take blows again (second step of the bonds note): the
  // fate of every bond and, per group, the damage taken and a blow left for the next tick.
  struct Remnant {
    float damage = 0, impulse = 0;
    bool pending = false;
    FractureHit hit = FractureHit::Impact;
    Tempest::Vec3 point, direction;
    };
  std::vector<bool> fragmentBondBroken;
  std::vector<Remnant> fragmentRemnant;
  uint32_t fragmentCollisionGroup = 0;
  std::function<void(bool,const std::vector<Tempest::Matrix4x4>&)> fractureCallback;
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

  Npc* toNpc() const {
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
        // Replacing or removing one frozen NPC shifts every later record.
        // onMove uses this cached index before the next tick can re-sort it.
        for(size_t j=i;j<arr.size();++j)
          if(arr[j].body!=nullptr)
            arr[j].body->frozen=j;
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

  bool hasCollision(const NpcBody& a, const NpcBody& b, Tempest::Vec3& normal, float& colDepth) const;

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
        if(wrld.bulletTrace!=nullptr)
          wrld.bulletTrace->forget(*i);
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
        if(wrld.bulletTrace!=nullptr)
          wrld.bulletTrace->write(i,i.cb,"moving_npc",0,npc.toNpc());
        const bool hit = precise ? i.cb->onCollidePrecise(*npc.toNpc(),i.lastPos,i.pos) : i.cb->onCollide(*npc.toNpc());
        if(wrld.bulletTrace!=nullptr)
          wrld.bulletTrace->write(i,i.cb,hit ? "moving_npc_hit" : "moving_npc_pass",0,npc.toNpc());
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

struct DynamicWorld::ItemHulls {
  std::unordered_map<std::string,ItemHull> shapes;
  };
