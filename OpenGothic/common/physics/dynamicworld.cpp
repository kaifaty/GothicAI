#include "dynamicworld_internal.h"

#include "collisionworld.h"
#include "collisioncache.h"
#include "physicmeshshape.h"
#include "physicvbo.h"
#include "ragdoll.h"
#include "cloth.h"
#include "worldwind.h"
#include "gothic.h"
#include "frameprofile.h"
#include "basestore.h"
#include "sha256.h"
#include <Tempest/Log>
#include "graphics/mesh/skeleton.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Geometry/ClosestPoint.h>

#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <chrono>
#include <stdexcept>
#include <cstdlib>

#include "graphics/mesh/submesh/packedmesh.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/bullet.h"
#include "world/world.h"
#include "game/serialize.h"
#include "game/physicssave.h"

const float DynamicWorld::worldHeight = 20000; //TODO: remove

namespace {
constexpr uint32_t waterMask     = 1u << DynamicWorld::C_Water;


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

struct DynamicWorld::ItemTrace {
  const ::Item* item = nullptr;
  bool enabled = true;
  uint64_t firstStep = 0, lastStep = 0, index = 0;
  std::vector<ItemTraceSample> samples;

  bool sample(CollisionWorld& world, bool beforeStep) {
    const auto step=world.simulationStep();
    for(const auto& native:world.waterBodySnapshot()) {
      if(native.body==nullptr || native.body->ptrType()!=P_Item || native.body->payload()!=item)
        continue;
      ItemTraceSample result;
      result.nativeStep=step;
      result.nativeSteps=step-lastStep;
      result.index=index++;
      result.seconds=float(step-firstStep)/60.f;
      result.stepSeconds=float(result.nativeSteps)/60.f;
      result.beforeStep=beforeStep;
      result.bodyId=native.nativeId;
      result.position=native.body->position();
      result.centerOfMass=native.body->centerOfMass();
      result.velocity=native.velocity;
      result.active=!native.sleeping;
      result.sleeping=native.sleeping;
      result.bounds[0]=native.collisionBounds[0];
      result.bounds[1]=native.collisionBounds[1];
      constexpr uint32_t mask=(1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_Item)|(1u<<C_Ragdoll)|
                              (1u<<C_LivingBody)|(1u<<C_ActiveBody)|(1u<<C_MovableVob)|(1u<<C_Fragment);
      for(const auto& contact:world.overlaps(*native.body,2.f,mask,true)) {
        ItemTraceContact entry;
        entry.category=contact.category;
        entry.depth=contact.depth;
        entry.normal=-contact.axis;
        entry.point=contact.point1;
        entry.otherPoint=contact.point2;
        entry.triangle=contact.triangle.valid;
        entry.nativeSubShapeId=contact.triangle.nativeSubShapeId;
        entry.sourceTriangle=contact.triangle.sourceTriangle;
        entry.triangleNormal=contact.triangle.normal;
        for(size_t i=0;i<3;++i) {
          entry.triangleVertices[i]=contact.triangle.vertices[i];
          entry.triangleEdgesCm[i]=contact.triangle.edgesCm[i];
          }
        if(contact.body!=nullptr) {
          entry.bodyId=contact.body->nativeId();
          entry.visual=contact.body->label();
          if(contact.body->ptrType()==P_MovableVob && contact.body->payload()!=nullptr)
            entry.zenId=static_cast<const BodyData*>(contact.body->payload())->pose->zenId;
          }
        result.contacts.push_back(std::move(entry));
        }
      for(const auto& solver:world.takeItemSolverContacts()) {
        if(solver.itemBodyId!=native.nativeId || solver.nativeStep<firstStep)
          continue;
        const auto& contact=solver.contact;
        ItemTraceContact entry;
        entry.nativeStep=solver.nativeStep;
        entry.persisted=solver.persisted;
        entry.bodyId=solver.bodyId;
        entry.category=contact.category;
        entry.depth=contact.depth;
        entry.normal=-contact.axis;
        entry.point=contact.point1;
        entry.otherPoint=contact.point2;
        entry.triangle=contact.triangle.valid;
        entry.nativeSubShapeId=contact.triangle.nativeSubShapeId;
        entry.sourceTriangle=contact.triangle.sourceTriangle;
        entry.triangleNormal=contact.triangle.normal;
        for(size_t i=0;i<3;++i) {
          entry.triangleVertices[i]=contact.triangle.vertices[i];
          entry.triangleEdgesCm[i]=contact.triangle.edgesCm[i];
        }
        if(contact.body!=nullptr) {
          entry.visual=contact.body->label();
          if(contact.body->ptrType()==P_MovableVob && contact.body->payload()!=nullptr)
            entry.zenId=static_cast<const BodyData*>(contact.body->payload())->pose->zenId;
        }
        result.solverContacts.push_back(std::move(entry));
      }
      for(const auto& sweep:world.takeItemSweepTrace()) {
        if(sweep.itemBodyId!=native.nativeId || sweep.nativeStep<firstStep)
          continue;
        ItemTraceSweep entry;
        entry.nativeStep=sweep.nativeStep;
        entry.itemBodyId=sweep.itemBodyId;
        entry.bodyId=sweep.bodyId;
        entry.category=sweep.category;
        entry.tested=sweep.tested;
        entry.hit=sweep.hit;
        entry.triggered=sweep.triggered;
        entry.status=sweep.status;
        entry.from=sweep.from;
        entry.to=sweep.to;
        entry.point=sweep.point;
        entry.normal=sweep.normal;
        entry.fraction=sweep.fraction;
        entry.beforeCm=sweep.beforeCm;
        entry.afterCm=sweep.afterCm;
        entry.triangle=sweep.triangle.valid;
        entry.nativeSubShapeId=sweep.triangle.nativeSubShapeId;
        entry.sourceTriangle=sweep.triangle.sourceTriangle;
        entry.triangleNormal=sweep.triangle.normal;
        for(size_t i=0;i<3;++i) {
          entry.triangleVertices[i]=sweep.triangle.vertices[i];
          entry.triangleEdgesCm[i]=sweep.triangle.edgesCm[i];
          }
        result.recoverySweeps.push_back(std::move(entry));
        }
      samples.push_back(std::move(result));
      lastStep=step;
      return true;
      }
    return false;
    }
  };

struct DynamicWorld::WorldBodies {
  std::vector<JPH::Vec3>                        vertices;
  std::vector<JPH::Vec3>                        clothVertices;
  std::unique_ptr<CollisionWorld::CollisionBody> landscape;
  std::unique_ptr<CollisionWorld::CollisionBody> water;
  std::unique_ptr<CollisionWorld::CollisionBody> originalCloth;
  };

DynamicWorld::DynamicWorld(World* owner, const asset::WorldMesh& worldMesh, const asset::WorldMesh* originalCloth) {
  world.reset(new CollisionWorld());
  ownerWorld=owner;
  if(owner!=nullptr)
    splitStaticTriangles=Gothic::settingsGetI("GAME","static_collision_split")!=0;
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
      damageMovableVob(*static_cast<BodyData*>(body.payload()),impulse,point,FractureHit::Impact,culprit);
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

  auto started=std::chrono::steady_clock::now();
  const auto elapsed=[&]() {
    const auto now=std::chrono::steady_clock::now();
    const auto milliseconds=std::chrono::duration<double,std::milli>(now-started).count();
    started=now;
    return milliseconds;
    };
  std::filesystem::path cacheRoot;
  CollisionCache::Key key;
  const auto* cacheEnabled=std::getenv("OPENGOTHIC_COLLISION_CACHE");
  if(owner!=nullptr && (cacheEnabled==nullptr || std::string_view(cacheEnabled)!="0")) {
    try {
      cacheRoot=BaseStore::cacheRoot();
      }
    catch(const std::exception&) {
      // An unavailable derived cache must not prevent building the collision.
      }
    const auto inputs=Resources::wayHeightInputs(owner->name());
    const auto& flags=Gothic::physicsFlags();
    key={inputs.baseFingerprint,std::filesystem::path(owner->name()).stem().string(),inputs.contentFingerprint,
         uint32_t(flags.cloth)|(uint32_t(flags.movableVobs)<<1)|(uint32_t(splitStaticTriangles)<<2)};
    }
  CollisionCache cache(cacheRoot,key);
  collisionLoadStats.keyMs=elapsed();
  JPH::ShapeRefC landscapeShape,waterShape;
  if(!cache.path().empty()) {
    collisionLoadStats.cacheHit=cache.read(landscapeShape,waterShape);
    collisionLoadStats.readMs=elapsed();
    }
  if(!collisionLoadStats.cacheHit) {
    landscapeShape=landMesh->shape(splitStaticTriangles);
    waterShape=waterMesh->shape();
    collisionLoadStats.buildMs=elapsed();
    if(!cache.path().empty()) {
      cache.save(landscapeShape.GetPtr(),waterShape.GetPtr());
      collisionLoadStats.writeMs=elapsed();
      }
    }

  // Cached shapes skip shape(), which normally prepares immutable source data.
  // Contact listeners may query it concurrently once the bodies are published.
  landMesh->prepareTriangleData();
  waterMesh->prepareTriangleData();
  JPH::Vec3 bbox[2] = {JPH::Vec3::sZero(), JPH::Vec3::sZero()};
  Tempest::Matrix4x4 mt;
  mt.identity();
  if(landscapeShape!=nullptr) {
    bodies->landscape = world->addCollisionBody(landscapeShape,mt,materialFriction(asset::MaterialGroup::NONE),
                                               C_Landscape,false,false,landMesh.get());
    bodies->landscape->setLabel("landscape");
    JPH::Vec3 min, max;
    bodies->landscape->getAabb(min,max);
    bbox[0] = JPH::Vec3::sMin(bbox[0],min);
    bbox[1] = JPH::Vec3::sMax(bbox[1],max);
    }

  if(waterShape!=nullptr) {
    bodies->water = world->addCollisionBody(waterShape,mt,0,C_Water,false,true,waterMesh.get());
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

DynamicWorld::RayLandResult DynamicWorld::floorRay(const Tempest::Vec3& from, float maxDy) const {
  world->updateAabbs();
  constexpr uint32_t floorMask = 1u << C_Landscape;
  const auto to = Tempest::Vec3(from.x,from.y-maxDy,from.z);
  return toResult(world->rayCast(from,to,floorMask,false),to);
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

DynamicWorld::RayLandResult DynamicWorld::rayStatic(const Tempest::Vec3& from, const Tempest::Vec3& to) const {
  constexpr uint32_t staticMask = (1u << C_Landscape) | (1u << C_Object);
  const auto hit = world->rayCast(from,to,staticMask,false);
  auto ret = toResult(hit, to);
  if(hit.hasHit)
    ret.n = hit.normal;
  return ret;
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
  if(shape==nullptr)
    return Item();
  const auto native=shape->mesh.shape(splitStaticTriangles);
  if(native==nullptr)
    return Item();
  auto obj = std::make_unique<BodyData>();
  obj->body = world->addCollisionBody(native,m,shape->friction(),C_Object,false,false,&shape->mesh);
  obj->body->setLabel(label);
  obj->body->setNoiseProfile(impactMaterial(shape->material),shape->material,"vob");
  return Item(this,obj.release());
  }

DynamicWorld::Item DynamicWorld::addMovableObj(const PhysicMeshShape* shape, const Tempest::Matrix4x4& m, std::string_view label) {
  if(shape==nullptr)
    return Item();
  const auto native=shape->mesh.shape();
  if(native==nullptr)
    return Item();
  auto obj = std::make_unique<BodyData>();
  obj->body = world->addCollisionBody(native,m,shape->friction(),C_Object,true,false,&shape->mesh);
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

void DynamicWorld::cycleBodyIds(size_t count) {
  world->cycleBodyIds(count);
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

void DynamicWorld::setBuoyancyEnabled(bool enabled) {
  world->setBuoyancyEnabled(enabled);
  }

void DynamicWorld::setItemWakeEnabled(bool enabled) {
  world->setItemWakeEnabled(enabled);
  }

void DynamicWorld::setItemDeepestContactEnabled(bool enabled) {
  world->setItemDeepestContactEnabled(enabled);
  }

void DynamicWorld::setRagdollBirthGuardEnabled(bool enabled) {
  world->setRagdollBirthGuardEnabled(enabled);
  }

bool DynamicWorld::buoyancyEnabled() const {
  return world->buoyancyEnabled();
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

std::vector<DynamicWorld::WaterBodySnapshot> DynamicWorld::waterBodySnapshot() const {
  std::vector<WaterBodySnapshot> result;
  std::unordered_map<const CollisionWorld::CollisionBody*,std::pair<const BodyData*,size_t>> identity;
  for(const auto* obj:movableVobs->bodies) {
    if(obj->body!=nullptr) identity.emplace(obj->body.get(),std::make_pair(obj,size_t(-1)));
    // A bond group reports its first piece.
    for(size_t i=0;i<obj->fragmentGroup.size();++i)
      if(const auto* body=obj->fragments[obj->fragmentGroup[i]].get(); body!=nullptr) identity.emplace(body,std::make_pair(obj,i));
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
        for(size_t i=0;i<obj->fragmentGroup.size();++i)
          if(obj->fragments[obj->fragmentGroup[i]].get()==contact.body && contact.body!=nullptr) {
            matches=true;
            piece=i; // the first piece of its bond group
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
        const auto group=obj->fragmentGroup[piece];
        const auto matrix=obj->fragments[group]->matrix();
        for(size_t member=piece;member<obj->fragmentGroup.size();++member) {
          if(obj->fragmentGroup[member]!=group) continue;
          for(const auto& face:obj->movableShape->fracture->pieces[member].triangles) {
            const Tempest::Vec3 vertices[]={face.vertices[0].position,face.vertices[1].position,face.vertices[2].position};
            triangle(vertices,matrix);
            }
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

void DynamicWorld::tick(uint64_t dt) {
  if(ownerWorld!=nullptr) {
    world->setBodyImpactEnabled(Gothic::physicsFlags().bodyImpactDamage);
    const float cycle=Gothic::settingsGetF("ENGINE","zWindCycleTime")+
                      Gothic::settingsGetF("ENGINE","zWindCycleTimeVar");
    const uint64_t period=std::isfinite(cycle) && cycle>0.f ? uint64_t(cycle*1000.f) : 0;
    clothWorld->setWind(WorldWind::sample(ownerWorld->tickCount(),period,
                                        Gothic::settingsGetI("ENGINE","zWindEnabled")!=0));
    }
  npcList   ->tickAabbs();
  bulletList->tick(dt);
  if(itemTrace && itemTrace->enabled) itemTrace->sample(*world,true);
  world     ->tick(dt);
  if(itemTrace && itemTrace->enabled) itemTrace->sample(*world,false);
  {
    FrameProfile::Scope timing(FrameProfile::Zone::Destruction);
    for(auto* obj:movableVobs->bodies)
      if(movableVobs->enabled || obj->className=="fence" || obj->pose->broken) {
        if(obj->pendingBreak) breakMovableVob(*obj); else if(obj->pose->broken) breakRemnants(*obj);
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
        if(auto* npc=record.body->toNpc(); npc!=nullptr && !npc->usesSimplifiedMovement())
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

DynamicWorld::BodyDigest DynamicWorld::bodyDigest() const {
  auto native=world->bodyDigest();
  Sha256 owners;
  const auto u32=[&](uint32_t value) { owners.update(&value,sizeof(value)); };
  size_t movables=0;
  if(movableVobs!=nullptr)
    for(const auto* vob:movableVobs->bodies) {
      ++movables;
      u32(vob->pose->zenId);
      u32(uint32_t(vob->visual.size()));
      owners.update(vob->visual.data(),vob->visual.size());
      u32(uint32_t(vob->pose->broken));
      u32(vob->body!=nullptr ? vob->body->nativeId() : uint32_t(-1));
      for(const auto& query:vob->query)
        u32(query->nativeId());
      }
  return {std::move(native.digest),owners.finish(),native.bodies,native.active,movables};
  }

DynamicWorld::StaticCollisionStats DynamicWorld::staticCollisionStats() const {
  const auto stats=world->staticCollisionStats();
  return {stats.mSizeBytes,stats.mNumTriangles};
  }

uint64_t DynamicWorld::simulationStep() const { return world->simulationStep(); }

bool DynamicWorld::startItemTrace(const ::Item& item) {
  auto trace=std::make_unique<ItemTrace>();
  trace->item=&item;
  trace->firstStep=trace->lastStep=world->simulationStep();
  if(!trace->sample(*world,false))return false;
  world->setItemTraceTarget(&item);
  itemTrace=std::move(trace);
  return true;
  }

std::vector<DynamicWorld::ItemTraceSample> DynamicWorld::takeItemTrace() {
  std::vector<ItemTraceSample> result;
  if(itemTrace)result.swap(itemTrace->samples);
  if(itemTrace && !itemTrace->enabled)itemTrace.reset();
  return result;
  }

void DynamicWorld::stopItemTrace() {
  if(itemTrace)itemTrace->enabled=false;
  world->setItemTraceTarget(nullptr);
  }

void DynamicWorld::configureMeleeBench(int substeps) { world->configureMeleeBench(substeps); }
void DynamicWorld::updateMeleeBlade(const Npc* owner,const Tempest::Matrix4x4& hand,float lengthCm) { world->updateMeleeBlade(owner,hand,lengthCm); }
void DynamicWorld::removeMeleeBlade(const Npc* owner) { world->removeMeleeBlade(owner); }
void DynamicWorld::updateShadowBody(Npc* owner,const std::vector<MeleeBodyCapsule>& capsules) { world->updateShadowBody(owner,capsules); }
void DynamicWorld::updateShadowBody(Npc* owner,const MeleeShadowBody& body,size_t* slotHint) { world->updateShadowBody(owner,body,slotHint); }
void DynamicWorld::boundShadowBody(MeleeShadowBody& body) { CollisionWorld::boundShadowBody(body); }
void DynamicWorld::removeShadowBody(const Npc* owner) { world->removeShadowBody(owner); }
bool DynamicWorld::shadowBody(const Npc* owner,MeleeShadowBody& out) const { return world->shadowBody(owner,out); }
void DynamicWorld::updateShadowBlade(const Npc* owner,uint64_t swingId,const std::vector<MeleeBladeSegment>& segments,
                                     uint64_t timeMs,double activeBeginMs,double activeEndMs,float walkableCos,
                                     Tempest::Vec3 forward, MeleeImpact::Weapon weapon) {
  world->updateShadowBlade(owner,swingId,segments,timeMs,activeBeginMs,activeEndMs,walkableCos,forward,weapon);
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
    case asset::MaterialGroup::CLAY:
      break;
    }
  return 0.75f;
  }

float DynamicWorld::materialDensity(asset::MaterialGroup mat) {
  switch (mat) {
    case asset::MaterialGroup::UNDEFINED:
    case asset::MaterialGroup::NONE:
    case asset::MaterialGroup::CLAY:
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
  CollisionWorld::CapsuleContact contact;
  if(out.trackBodyImpact) {
    contact=[&](CollisionWorld::CollisionBody* body,const Tempest::Vec3& normal,const Tempest::Vec3& point,
                const Tempest::Vec3& nativeVelocity) {
      auto velocity=nativeVelocity;
      if(body!=nullptr && body->ptrType()==P_MovableVob && body->payload()!=nullptr) {
        const auto* movable=static_cast<const BodyData*>(body->payload());
        if(movable->body!=nullptr)
          velocity=movable->body->velocity()+Tempest::Vec3::crossProduct(movable->body->angularVelocity(),
                                                                       point-movable->body->centerOfMass());
        }
      out.bodyImpactSpeed=std::max(out.bodyImpactSpeed,
          -Tempest::Vec3::dotProduct(out.bodyImpactVelocity-velocity*.001f,normal));
      };
    }
  if(it.obj->shape!=nullptr && world->hasCollision(it.obj->shape.GetPtr(),it.obj->centerPosition(),out.normal,out.vob,contact)) {
    out.landCol = true;
    ret = true;
    }

  if(!ret)
    return false;
  out.normal = Tempest::Vec3::normalize(out.normal);
  return true;
  }

uint32_t DynamicWorld::Item::nativeId() const {
  return obj!=nullptr && obj->body!=nullptr ? obj->body->nativeId() : uint32_t(-1);
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
  if(obj->movableShape!=nullptr) owner->damageMovableVob(*obj,impulseNs.length(),obj->body->centerOfMass(),FractureHit::Impact,{},impulseNs);
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
