#include "dynamicworld_internal.h"

#include "cloth.h"
#include "physicshost.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>

DynamicWorld::BulletBody* DynamicWorld::bulletObj(BulletCallback* cb) {
  return bulletList->add(cb);
  }

DynamicWorld::BBoxBody DynamicWorld::bboxObj(BBoxCallback* cb, const asset::Aabb& bbox) {
  return BBoxBody(this,cb,bbox);
  }

DynamicWorld::BBoxBody DynamicWorld::bboxObj(BBoxCallback* cb, const Tempest::Vec3& pos, float R) {
  return BBoxBody(this,cb,pos,R);
  }

void DynamicWorld::moveBullet(BulletBody &b, const Tempest::Vec3& dir, uint64_t dt) {
  if(bulletTrace!=nullptr)
    bulletTrace->write(b,b.cb,"begin",dt);
  const float dtF     = float(dt);
  const bool  isSpell = b.isSpell();

  auto  pos = b.pos;
  auto  to  = pos + dir*dtF - Tempest::Vec3(0,(isSpell ? 0 : 0.5f*gravity*dtF*dtF),0);

  if(auto ptr = bboxList->rayTest(pos,to)) {
    if(ptr->cb!=nullptr)
      ptr->cb->onCollide(b);
    }
  const bool attachAtWorldStop=!isSpell && b.cb!=nullptr && b.cb->stopsAtWorldHit();
  auto hit = world->rayCast(pos,to,visualMask,false);
  if(!isSpell && world->itemWakeEnabled()) {
    // Bodyless resting Items must become queryable before the projectile ray.
    // Stop the wake volume at the solid world hit, so walls shield Items too.
    const auto end=hit.hasHit ? hit.point : to;
    const Tempest::Vec3 min(std::min(pos.x,end.x)-50.f,std::min(pos.y,end.y)-50.f,std::min(pos.z,end.z)-50.f);
    const Tempest::Vec3 max(std::max(pos.x,end.x)+50.f,std::max(pos.y,end.y)+50.f,std::max(pos.z,end.z)+50.f);
    world->wakeItemsInBounds(min,max);
    hit=world->rayCast(pos,to,visualMask|(1u<<C_Item),false);
    }
  if(!isSpell && movableVobs->destructionEnabled) {
    // An arrow stops at a remnant before the world behind it; single pieces it passes, as before.
    std::vector<CollisionWorld::QueryResult> debris;
    world->rayCast(pos,to,fragmentMask,false,&debris);
    uint32_t group=0;
    for(const auto& entry:debris)
      if((!hit.hasHit || entry.fraction<hit.fraction) && remnantOf(reinterpret_cast<uintptr_t>(entry.body),group)!=nullptr)
        hit=entry;
    }
  const auto matId = hit.hasHit ? hit.material : asset::MaterialGroup::NONE;
  if(bulletTrace!=nullptr)
    bulletTrace->write(b,b.cb,"world_query",dt,nullptr,hit.hasHit,hit.fraction,hit.point,hit.normal,
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
    else if(host!=nullptr) {
      const auto kind=host->physicalSpell(b.spellId());
      if(kind==PhysicsHost::PhysicalSpell::Fireball || kind==PhysicsHost::PhysicalSpell::WindFist)
        clothWorld->projectile(pos,end,Tempest::Vec3::normalize(incoming)*(kind==PhysicsHost::PhysicalSpell::WindFist ? 8.f : 5.f),false);
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
      const bool boneHit=hasCloth && host->npcRayHitZone(npc,pos,end,distance)!=HitZones::None;
      if(!b.cb->onCollidePrecise(npc,pos,end))
        return false;
      const auto impact=boneHit ? pos+Tempest::Vec3::normalize(end-pos)*distance : end;
      notifyCloth(impact);
      return true;
      })) {
      b.cb->onStop();
      if(bulletTrace!=nullptr)
        bulletTrace->write(b,b.cb,"end",dt);
      return;
      }
    }

  bool stopBullet = false;
  if(matId != asset::MaterialGroup::NONE) {
    notifyCloth(pos+(to-pos)*hit.fraction);
    if(!isSpell && world->itemWakeEnabled() && hit.body!=nullptr && hit.body->category()==C_Item) {
      auto* culprit=b.cb!=nullptr ? b.cb->origin() : nullptr;
      hit.body->addImpulseAtPoint(b.direction()*1.2f,hit.point,culprit);
      }
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
        damageMovableVob(target,impulse.length(),hit.point,FractureHit::Arrow,{},b.direction());
        }
      }
    else if(!isSpell && hit.body!=nullptr && hit.body->ptrType()==P_Fragment)
      arrowRemnant(reinterpret_cast<uintptr_t>(hit.body),hit.point,b.direction(),b.cb!=nullptr ? b.cb->origin() : nullptr);
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
      if(bulletTrace!=nullptr)
        bulletTrace->write(b,b.cb,"npc_collision",dt,ptr->toNpc());
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
    if(bulletTrace!=nullptr)
      bulletTrace->write(b,b.cb,"stop",dt,nullptr,hit.hasHit,hit.fraction,hit.point,hit.normal,
                      unsigned(matId),hit.body!=nullptr ? unsigned(hit.body->category()) : 0);
    if(b.cb!=nullptr) {
      // Pin only after the unchanged flight code has decided to stop at a solid
      // world contact. In particular, stone/metal still reflect before this test.
      // Items have no attachment pose; do not pin their stopped arrow to the world.
      // Nor to debris: a remnant moves and breaks under it.
      if(attachAtWorldStop && matId!=asset::MaterialGroup::NONE &&
         (hit.body==nullptr || (hit.body->category()!=C_Item && !isFragment(hit.body->category())))) {
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
  if(bulletTrace!=nullptr)
    bulletTrace->write(b,b.cb,"end",dt);
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

DynamicWorld::BBoxBody::BBoxBody(DynamicWorld* ow, BBoxCallback* cb, const asset::Aabb& bbox)
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
