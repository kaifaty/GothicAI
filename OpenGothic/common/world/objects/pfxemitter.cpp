#include "pfxemitter.h"
#include "loadprobe.h"

#include <Tempest/Log>

#include "graphics/pfx/pfxobjects.h"
#include "graphics/pfx/particlefx.h"

#include "world/world.h"
#include "world/objects/npc.h"
#include "world/voblayer.h"
#include "utils/fileext.h"
#include "gothic.h"

using namespace Tempest;

PfxEmitter::PfxEmitter(World& world, std::string_view name)
  :PfxEmitter(world,Gothic::inst().loadParticleFx(name)) {
  }

PfxEmitter::PfxEmitter(World& world, const ParticleFx* decl) {
  if(Resources::isHeadless()) {
    cpuDecl = decl;
    cpuWorld = &world;
    return;
    }
  *this = PfxEmitter(world.view()->pfxGroup,decl);
  }

PfxEmitter::PfxEmitter(PfxObjects& owner, const ParticleFx* decl)
  :slot(owner,decl) {
  }

PfxEmitter::PfxEmitter(World& world, const asset::VirtualObject& vob) {
  LoadProbe::Vobs::Scope probe("pfx");
  if(Resources::isHeadless()) {
    if(FileExt::hasExt(vob.visual->name,"PFX")) {
      cpuDecl = Gothic::inst().loadParticleFx(vob.visual->name);
      cpuWorld = &world;
      }
    return;
    }
  auto& owner = world.view()->pfxGroup;
  if(FileExt::hasExt(vob.visual->name,"PFX")) {
    auto decl = Gothic::inst().loadParticleFx(vob.visual->name);
    if(decl==nullptr || decl->visMaterial.tex==nullptr)
      return;
    slot = PfxSlot(owner,*decl,VobLayer::flicker(vob));
    }
  else if(auto decal = dynamic_cast<const asset::VisualDecal*>(vob.visual.get())) {
    Material mat(*decal);
    slot = PfxSlot(owner,mat,vob);
    }
  }

PfxEmitter::~PfxEmitter() {
  }

PfxEmitter::PfxEmitter(PfxEmitter && b)
  :PfxEmitter() {
  *this = std::move(b);
  }

PfxEmitter& PfxEmitter::operator=(PfxEmitter &&b) {
  slot = std::move(b.slot);
  std::swap(zone,  b.zone);
  std::swap(cpuDecl,b.cpuDecl);
  std::swap(cpuWorld,b.cpuWorld);
  std::swap(cpuPosition,b.cpuPosition);
  std::swap(cpuActive,b.cpuActive);
  std::swap(cpuLooped,b.cpuLooped);
  std::swap(cpuStarted,b.cpuStarted);
  std::swap(cpuStopped,b.cpuStopped);
  return *this;
  }

void PfxEmitter::setPosition(float x, float y, float z) {
  setPosition(Vec3(x,y,z));
  }

void PfxEmitter::setPosition(const Vec3& pos) {
  if(cpuDecl!=nullptr) {
    cpuPosition = pos;
    zone.setPosition(pos);
    return;
    }
  if(slot.isEmpty())
    return;
  auto guard = slot.lock();
  slot.setPosition(pos);
  zone.setPosition(pos);
  }

bool PfxEmitter::renderPosition(Vec3& out) const {
  return slot.renderPosition(out);
  }

void PfxEmitter::setTarget(const Npc* tg) {
  slot.setTarget(tg);
  }

void PfxEmitter::setDirection(const Matrix4x4& d) {
  slot.setDirection(d);
  }

void PfxEmitter::setActive(bool act) {
  if(cpuDecl!=nullptr) {
    if(cpuActive!=act) {
      if(act)
        cpuStarted = cpuWorld->tickCount();
      else
        cpuStopped = cpuWorld->tickCount();
      cpuActive = act;
      }
    return;
    }
  slot.setActive(act);
  }

bool PfxEmitter::isActive() const {
  if(cpuDecl!=nullptr)
    return cpuActive;
  return slot.isActive();
  }

void PfxEmitter::setLooped(bool loop) {
  if(cpuDecl!=nullptr) {
    cpuLooped = loop;
    return;
    }
  slot.setLooped(loop);
  }

void PfxEmitter::setMesh(const MeshObjects::Mesh* mesh, const Pose* pose, const std::vector<Tempest::Matrix4x4>* skin) {
  slot.setMesh(mesh,pose,skin);
  }

void PfxEmitter::setPhysicsEnable(World& p, std::function<void (Npc&)> cb) {
  if(cpuDecl!=nullptr) {
    zone = CollisionZone(p,cpuPosition,*cpuDecl);
    zone.setCallback(std::move(cb));
    return;
    }
  if(slot.isEmpty())
    return;
  auto guard = slot.lock();
  zone = CollisionZone(p, slot.position(), slot.decl());
  zone.setCallback(cb);
  }

void PfxEmitter::setPhysicsDisable() {
  zone = CollisionZone();
  }

uint64_t PfxEmitter::effectPrefferedTime() const {
  if(cpuDecl!=nullptr)
    return cpuDecl->effectPrefferedTime();
  if(slot.isEmpty())
    return 0;
  return slot.decl().effectPrefferedTime();
  }

bool PfxEmitter::isAlive() const {
  if(cpuDecl!=nullptr) {
    const uint64_t now = cpuWorld->tickCount();
    if(cpuActive && cpuLooped)
      return true;
    const uint64_t until = cpuActive ? cpuStarted+cpuDecl->effectPrefferedTime()+cpuDecl->maxLifetime()
                                    : cpuStopped+cpuDecl->maxLifetime();
    return now<until;
    }
  return slot.isAlive();
  }

void PfxEmitter::setObjMatrix(const Matrix4x4 &mt) {
  setPosition (mt.at(3,0),mt.at(3,1),mt.at(3,2));
  setDirection(mt);
  slot.setShapeMatrix(mt);
  }
