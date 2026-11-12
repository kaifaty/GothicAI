#include "bullet.h"

#include <Tempest/Log>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "graphics/visualfx.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world.h"
#include "testbench.h"
#include "gothic.h"
#include "settings.h"

using namespace Tempest;

Bullet::Bullet(World& owner, const Item& itm, const Vec3& pos)
  :wrld(&owner) {
  visualName = itm.handle().visual;
  obj = wrld->physic()->bulletObj(this);
  if(itm.isSpellOrRune()) {
    obj->setSpellId(itm.spellId());
    }

  if(itm.isSpellOrRune()) {
    material   = ItemMaterial::MAT_COUNT;
    int32_t id = itm.spellId();
    const VisualFx* vfx = owner.script().spellVfx(id);
    if(vfx!=nullptr) {
      auto e = Effect(*vfx,owner,pos,SpellFxKey::Cast);
      setView(std::move(e));
      }
    } else {
    material = uint8_t(itm.handle().material);
    setView(owner.addView(itm.handle()));
    }

  setPosition(pos);
  }

Bullet::~Bullet() {
  vfx.setBullet(nullptr,*wrld);
  wrld->physic()->deleteObj(obj);
  }

void Bullet::setPosition(const Tempest::Vec3& p) {
  obj->setPosition(p);
  updateMatrix();
  }

void Bullet::setPosition(float x, float y, float z) {
  obj->setPosition(Vec3(x,y,z));
  updateMatrix();
  }

void Bullet::setDirection(const Tempest::Vec3& dir) {
  obj->setDirection(dir);
  updateMatrix();
  }

void Bullet::setTargetRange(float t) {
  obj->setTargetRange(t);
  }

void Bullet::setView(MeshObjects::Mesh &&m) {
  view = std::move(m);
  updateMatrix();
  }

void Bullet::setView(Effect &&p) {
  vfx = std::move(p);
  vfx.setActive(true);
  vfx.setLooped(true);
  vfx.setBullet(this,*wrld);
  vfx.setPhysicsDisable();
  updateMatrix();
  }

bool Bullet::isSpell() const {
  return obj->isSpell();
  }

int32_t Bullet::spellId() const {
  return obj->spellId();
  }

void Bullet::setOrigin(Npc *n) {
  ow = n;
  vfx.setOrigin(n);
  }

Npc *Bullet::origin() const {
  return ow;
  }

void Bullet::setTarget(const Npc* n) {
  vfx.setTarget(n);
  }

ItemMaterial Bullet::itemMaterial() const {
  if(material>=ItemMaterial::MAT_COUNT)
    return ItemMaterial::MAT_WOOD;
  return ItemMaterial(material);
  }

bool Bullet::isFinished() const {
  if((flags()&Bullet::Stopped)!=Bullet::Stopped)
    return false;
  return true;
  }

float Bullet::pathLength() const {
  return obj->pathLength();
  }

bool Bullet::onEffectCollide(Npc& other) {
  const bool ret = onCollide(other);
  if(ret)
    onStop();
  return ret;
  }

void Bullet::onStop() {
  if(precise && zone==HitZones::None && !landed && !isFinished()) {
    const auto p = obj->position();
    char buf[160];
    const auto by = TestBench::who(origin());
    std::snprintf(buf,sizeof(buf),"arrow missed: flew out at %.0f %.0f %.0f, flew %.0f cm, by %.*s",p.x,p.y,p.z,double(pathLength()),
                  int(by.size()),by.data());
    TestBench::note(buf);
    }
  flg = Flg(flg|Stopped);
  updateMatrix();
  }

Tempest::Vec3 Bullet::position() const {
  return obj!=nullptr ? obj->position() : Tempest::Vec3();
  }

Tempest::Vec3 Bullet::direction() const {
  return obj!=nullptr ? obj->direction() : Tempest::Vec3();
  }

void Bullet::onMove() {
  updateMatrix();
  if(!isSpell() && TestBench::isActive()) {
    const auto p = position();
    TestBench::arrowAt(this,p.x,p.y,p.z);
    }
  }

bool Bullet::stopsAtWorldHit() const {
  return !isSpell() && Gothic::physicsFlags().arrowSticking;
  }

void Bullet::onSpellWorldHit(const DynamicWorld::BulletWorldHit& hit) {
  impactAt = hit.point;
  impactOwner = hit.bone;
  impactKnown = true;
  onCollide(hit.material);
  }

void Bullet::onWorldHit(const DynamicWorld::BulletWorldHit& hit) {
  TestBench::ownMeshArrowHit(hit.movable!=nullptr ? hit.movable->zenId : uint32_t(-1),hit.point,hit.normal);
  impactAt = hit.point;
  impactOwner = hit.bone;
  impactKnown = true;
  auto matrix = obj->matrix();
  if(auto* npc = wrld->npcForRagdoll(hit.ragdoll)) {
    npc->addCorpseArrowImpulse(*this);
    if(npc->stickArrow(std::move(view),hit.bone,matrix,visualName)) {
      char buf[160];
      const auto who = TestBench::who(npc);
      std::snprintf(buf,sizeof(buf),"arrow stuck corpse %.*s bone %zu",int(who.size()),who.data(),hit.bone);
      TestBench::note(buf);
      return;
      }
    }
  auto sightNormal = hit.normal;
  if(Tempest::Vec3::dotProduct(sightNormal,hit.incoming)>0.f)
    sightNormal = -sightNormal;
  const auto sightPoint = hit.point+sightNormal*3.f;
  const auto surface = hit.vob!=nullptr && hit.vob->isDoor() ? "door" :
                       (std::abs(hit.normal.y)<.7f ? "wall" : "ground");
  if(wrld->stickArrow(std::move(view),matrix,visualName,hit.movable,origin(),&sightPoint,surface)) {
    char buf[256];
    std::snprintf(buf,sizeof(buf),"arrow stuck world: at %.9g %.9g %.9g normal %.9g %.9g %.9g material %u",
                  double(hit.point.x),double(hit.point.y),double(hit.point.z),
                  double(hit.normal.x),double(hit.normal.y),double(hit.normal.z),unsigned(hit.material));
    TestBench::note(buf);
    }
  }

void Bullet::onCollide(asset::MaterialGroup matId) {
  if(isFinished())
    return;
  applySpellPhysics(nullptr);
  if(precise && zone==HitZones::None && !landed) {
    // where it came down first: it may glance off stone and fly on
    landed = true;
    const auto p = obj->position();
    char buf[160];
    const auto by = TestBench::who(origin());
    std::snprintf(buf,sizeof(buf),"arrow missed: came down at %.0f %.0f %.0f, flew %.0f cm, by %.*s",p.x,p.y,p.z,double(pathLength()),
                  int(by.size()),by.data());
    TestBench::note(buf);
    }
  if(matId != asset::MaterialGroup::NONE) {
    if(material < ItemMaterial::MAT_COUNT) {
      auto s = wrld->addLandHitEffect(ItemMaterial(material),matId,obj->matrix());
      s.play();
      }
    }
  Effect::onCollide(*wrld,vfx.handle(),obj->position(),nullptr,ow,spellId());
  vfx.setLooped(false);
  vfx.setPhysicsDisable();
  wrld->runEffect(std::move(vfx));
  }

bool Bullet::onCollidePrecise(Npc& npc, const Vec3& from, const Vec3& to) {
  if(&npc==origin() || isFinished() || &npc==passedDodger)
    return false;
  // between the arm and the trunk, over the head: past it
  float t = 0;
  size_t bone = size_t(-1);
  const auto z = npc.rayHitZone(from,to,t,&bone);
  if(z==HitZones::None)
    return false;
  zone = z;
  impactOwner = bone;
  impactAt = from+Vec3::normalize(to-from)*t;
  impactKnown = true;
  const int32_t hp = npc.attribute(ATR_HITPOINTS);
  const bool    ret = onCollide(npc);
  if(!ret && &npc==passedDodger) {
    if(TestBench::isActive()) {
      const auto who = TestBench::who(&npc);
      const auto by  = TestBench::who(origin());
      char buf[256];
      std::snprintf(buf,sizeof(buf),"arrow passed %.*s: inside its dodge window, flies on, by %.*s",
                    int(who.size()),who.data(),int(by.size()),by.data());
      TestBench::note(buf);
      }
    return false;
    }
  if(TestBench::isActive()) {
    const auto p   = from + Vec3::normalize(to-from)*t;
    const auto who = TestBench::who(&npc);
    const auto by  = TestBench::who(origin());
    char buf[256];
    std::snprintf(buf,sizeof(buf),"arrow hit %.*s: %.*s, hp %d -> %d, draw %.2f, flew %.0f cm, at %.0f %.0f %.0f, by %.*s",
                  int(who.size()),who.data(),int(HitZones::name(z).size()),HitZones::name(z).data(),
                  int(hp),int(npc.attribute(ATR_HITPOINTS)),double(drawK),double(pathLength()+t),p.x,p.y,p.z,
                  int(by.size()),by.data());
    TestBench::note(buf);
    }
  return ret;
  }

bool Bullet::onCollide(Npc& npc) {
  if(&npc==origin() || isFinished() || &npc==passedDodger)
    return false;

  // An arrow or a bolt into a dodger inside the dodge's window: takeDamage refuses it (and logs it); it is not
  // pinned into the body and does not stop, it flies on as a miss. A spell still bursts there, harmless.
  const bool passes = !isSpell() && npc.isMeleeDodgeInvulnerable();
  if(ow!=nullptr) {
    // no damage between ally npc's, only emit pfx effect
    const bool friendlyFire = wrld->script().isFriendlyFire(*ow, npc);
    if(!friendlyFire) {
      if(isSpell())
        npc.takeDamage(*ow,this,vfx.handle(),spellId()); else
        npc.takeDamage(*ow,this);
      }
    }
  if(passes) {
    passedDodger = &npc;
    zone         = HitZones::None;
    impactOwner  = size_t(-1);
    impactKnown  = false;
    return false;
    }
  applySpellPhysics(&npc);
  vfx.setKey(*wrld,SpellFxKey::Collide);
  vfx.setLooped(false);
  vfx.setPhysicsDisable();
  wrld->runEffect(std::move(vfx));

  if(stopsAtWorldHit() && !impactKnown) {
    // Damage and the existing physical reaction consumed the legacy impact
    // metadata above. Resolve this additional frame only for the visual pin.
    const auto from = position();
    const auto dir = direction();
    float distance = 0;
    size_t bone = size_t(-1);
    if(dir.length()>1.e-3f &&
       npc.rayHitZone(from,from+Vec3::normalize(dir)*500.f,distance,&bone)!=HitZones::None) {
      impactOwner = bone;
      impactAt = from+Vec3::normalize(dir)*distance;
      }
    else {
      impactAt = npc.collosionCenter();
      }
    impactKnown = true;
    }

  if(stopsAtWorldHit()) {
    auto matrix = obj->matrix();
    const auto point = impactPoint();
    matrix.set(3,0,point.x);
    matrix.set(3,1,point.y);
    matrix.set(3,2,point.z);
    if(npc.stickArrow(std::move(view),impactOwner,matrix,visualName)) {
      Npc::ArrowPinProbe pin;
      npc.stuckArrowProbe(npc.stuckArrowCount()-1,pin);
      char buf[160];
      const auto who = TestBench::who(&npc);
      std::snprintf(buf,sizeof(buf),"arrow stuck npc %.*s bone %zu",int(who.size()),who.data(),pin.bone);
      TestBench::note(buf);
      }
    }
  return true;
  }

void Bullet::updateMatrix() {
  if(obj==nullptr)
    return;
  auto mat = obj->matrix();
  view.setObjMatrix(mat);
  // HACK: lighting bolt spell
  mat.rotateOY(90);
  vfx.setObjMatrix(mat);
  // vfx .setTarget(obj->position()+obj->direction());
  }

void Bullet::applySpellPhysics(Npc* target) {
  if(!isSpell() || spellPhysicsApplied || !Gothic::physicsFlags().spellPhysics)
    return;
  const auto kind = wrld->script().physicalSpell(spellId());
  if(kind!=GameScript::PhysicalSpell::WindFist && kind!=GameScript::PhysicalSpell::Fireball)
    return;
  spellPhysicsApplied = true;
  const auto point = target!=nullptr ? target->collosionCenter() : impactPoint();
  wrld->applySpellImpact(spellId(),point,origin(),target);
  }

// Opt-in flight evidence; observes the queries and callbacks used by the actual
// projectile instead of issuing extra collision tests or consuming random draws.
namespace ArrowTrace {
struct Output {
  Output() {
    const auto* path=std::getenv("OPENGOTHIC_ARROW_TRACE");
    if(path!=nullptr && path[0]!='\0')
      file=std::fopen(path,"w");
    if(file!=nullptr)
      std::fputs("id,step,game_ms,phase,dt,x,y,z,dx,dy,dz,path_cm,bounces,range_cm,spell,precise,stopped,origin,target,target_x,target_y,target_z,npc,npc_x,npc_y,npc_z,has_hit,fraction,hit_x,hit_y,hit_z,nx,ny,nz,material,category,reflect_threshold,hit_limit,path_limit,living_bodies,arrow_physics,arrow_sticking\n",file);
    }
  ~Output() { if(file!=nullptr) std::fclose(file); }
  std::FILE* file=nullptr;
  uint64_t next=0;
  std::unordered_map<const void*,uint64_t> ids;
  };

inline Output& output() {
  static Output value;
  return value;
  }

struct Writer final : DynamicWorld::BulletTrace {
void forget(const DynamicWorld::BulletBody& body) override {
  auto& out=output();
  if(out.file!=nullptr) out.ids.erase(&body);
  }

void write(const DynamicWorld::BulletBody& body, DynamicWorld::BulletCallback* callback,
           const char* phase, uint64_t dt, const void* npcPtr,
           bool hasHit, float fraction,
           Tempest::Vec3 hit, Tempest::Vec3 normal, unsigned material, unsigned category) override {
  auto& out=output();
  if(out.file==nullptr) return;
  const auto* npc=static_cast<const Npc*>(npcPtr);
  const auto* bullet=dynamic_cast<const Bullet*>(callback);
  auto* origin=bullet!=nullptr ? bullet->origin() : nullptr;
  const auto* target=origin!=nullptr ? origin->target() : nullptr;
  auto [entry,inserted]=out.ids.try_emplace(&body,0);
  if(inserted) entry->second=++out.next;
  const auto p=body.position(), d=body.direction();
  const auto tp=target!=nullptr ? target->collosionCenter() : Tempest::Vec3{};
  const auto np=npc!=nullptr ? npc->collosionCenter() : Tempest::Vec3{};
  const auto by=TestBench::who(origin), at=TestBench::who(target), who=TestBench::who(npc);
  std::fprintf(out.file,"%llu,%llu,%llu,%s,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%d,%d,%d,%.*s,%.*s,%.9g,%.9g,%.9g,%.*s,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%.9g,%d,%d,%d,%d,%d\n",
    static_cast<unsigned long long>(entry->second),static_cast<unsigned long long>(TestBench::stepCount()),
    static_cast<unsigned long long>(origin!=nullptr ? origin->world().tickCount() : 0),phase,
    static_cast<unsigned long long>(dt),double(p.x),double(p.y),double(p.z),
    double(d.x),double(d.y),double(d.z),double(body.pathLength()),body.hitCount(),double(body.targetRange()),
    int(body.isSpell()),int(callback!=nullptr && callback->isPrecise()),int(bullet!=nullptr && bullet->isFinished()),
    int(by.size()),by.data(),int(at.size()),at.data(),double(tp.x),double(tp.y),double(tp.z),
    int(who.size()),who.data(),double(np.x),double(np.y),double(np.z),int(hasHit),double(fraction),
    double(hit.x),double(hit.y),double(hit.z),double(normal.x),double(normal.y),double(normal.z),material,category,
    double(body.speed()*fraction),int(body.hitCount()>3),int(body.pathLength()>10000),
    Settings::getI("GAME","living_bodies"),Settings::getI("GAME","arrow_physics"),
    Settings::getI("GAME","arrow_sticking"));
  std::fflush(out.file);
  }
};

DynamicWorld::BulletTrace* hook() {
  const auto* path=std::getenv("OPENGOTHIC_ARROW_TRACE");
  if(path==nullptr || path[0]=='\0') return nullptr;
  static Writer value;
  return &value;
  }
}
