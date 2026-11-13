#include "pfxslot.h"
#include "loadprobe.h"

#include "graphics/mesh/submesh/pfxemittermesh.h"
#include "graphics/pfx/pfxobjects.h"
#include "graphics/pfx/pfxbucket.h"
#include "graphics/pfx/particlefx.h"
#include "graphics/worldview.h"

using namespace Tempest;

PfxSlot::PfxSlot(PfxObjects& owner, const ParticleFx* decl) {
  LoadProbe::Vobs::Scope probe("pfx");
  if(decl==nullptr || (decl->visMaterial.tex==nullptr && !decl->hasTrails()))
    return;
  std::lock_guard<std::recursive_mutex> guard(owner.sync);
  bucket = &owner.getBucket(*decl);
  id     = bucket->allocEmitter();
  if(decl->shpMesh!=nullptr && decl->shpMeshRender)
    shpMesh = owner.world.addView(decl->shpMesh_S,0,0,0);
  }

PfxSlot::PfxSlot(PfxObjects& owner, const ParticleFx& decl, const PfxMotion::Flicker* flicker) {
  std::lock_guard<std::recursive_mutex> guard(owner.sync);
  bucket = &owner.getBucket(decl);
  id     = bucket->allocEmitter();
  if(flicker!=nullptr)
    bucket->get(id).flicker = *flicker;
  }

PfxSlot::PfxSlot(PfxObjects& owner, const Material& mat, const asset::VirtualObject& vob) {
  std::lock_guard<std::recursive_mutex> guard(owner.sync);
  bucket = &owner.getBucket(mat,vob);
  id     = bucket->allocEmitter();
  }

PfxSlot::~PfxSlot() {
  if(bucket!=nullptr) {
    std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
    bucket->freeEmitter(id);
    }
  }

PfxSlot::PfxSlot(PfxSlot && b)
  :PfxSlot() {
  *this = std::move(b);
  }

PfxSlot& PfxSlot::operator=(PfxSlot &&b) {
  // the shape mesh stays with its slot, as it did with PfxEmitter's move
  std::swap(bucket,b.bucket);
  std::swap(id,    b.id);
  return *this;
  }

std::unique_lock<std::recursive_mutex> PfxSlot::lock() const {
  return std::unique_lock<std::recursive_mutex>(bucket->parent.sync);
  }

const ParticleFx& PfxSlot::decl() const {
  return bucket->decl;
  }

Vec3 PfxSlot::position() const {
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  return bucket->get(id).pos;
  }

void PfxSlot::setPosition(float x, float y, float z) {
  setPosition(Vec3(x,y,z));
  }

void PfxSlot::setPosition(const Vec3& pos) {
  if(bucket==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  v.pos = pos;
  if(bucket->decl.isDecal())
    bucket->updateTextureDemand(id);

  if(v.next!=nullptr)
    v.next->setPosition(pos);
  if(v.block==size_t(-1))
    return; // no backup memory
  auto& p = bucket->getBlock(*this);
  p.pos = pos;
  bucket->invalidateDecal();
  }

bool PfxSlot::renderPosition(Vec3& out) const {
  if(bucket==nullptr)
    return false;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  out = v.block==size_t(-1) ? v.pos : bucket->getBlock(v).pos;
  return true;
  }

void PfxSlot::setTarget(const ParticleTarget* tg) {
  if(bucket==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  v.targetNpc = tg;
  }

void PfxSlot::setDirection(const Matrix4x4& d) {
  if(bucket==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  v.direction[0] = Vec3(d.at(0,0),d.at(0,1),d.at(0,2));
  v.direction[1] = Vec3(d.at(1,0),d.at(1,1),d.at(1,2));
  v.direction[2] = Vec3(d.at(2,0),d.at(2,1),d.at(2,2));

  if(v.next!=nullptr)
    v.next->setDirection(d);
  }

void PfxSlot::setActive(bool act) {
  if(bucket==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v     = bucket->get(id);
  auto  state = (act ? PfxBucket::S_Active : PfxBucket::S_Inactive);
  if(v.st==state)
    return;
  v.st = state;
  if(v.next!=nullptr)
    v.next->setActive(act);
  if(act==true)
    v.waitforNext = bucket->decl.ppsCreateEmDelay;
  }

bool PfxSlot::isActive() const {
  if(bucket==nullptr)
    return false;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  return v.st==PfxBucket::S_Active;
  }

void PfxSlot::setLooped(bool loop) {
  if(bucket==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  v.isLoop = loop;
  if(v.next!=nullptr)
    v.next->setLooped(loop);
  }

void PfxSlot::setMesh(const MeshObjects::Mesh* mesh, const Pose* pose, const std::vector<Tempest::Matrix4x4>* skin) {
  if(bucket==nullptr)
    return;
  const PfxEmitterMesh* m = (mesh!=nullptr) ? mesh->toMeshEmitter() : nullptr;

  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  v.mesh = m;
  v.pose = pose;
  v.skin = skin;
  if(v.next!=nullptr)
    v.next->setMesh(mesh,pose,skin);
  }

bool PfxSlot::isAlive() const {
  if(bucket==nullptr)
    return false;
  std::lock_guard<std::recursive_mutex> guard(bucket->parent.sync);
  auto& v = bucket->get(id);
  if(v.block==size_t(-1))
    return false;
  auto& b = bucket->block[v.block];
  return b.count>0;
  }

void PfxSlot::setShapeMatrix(const Matrix4x4 &mt) {
  shpMesh.setObjMatrix(mt);
  }
