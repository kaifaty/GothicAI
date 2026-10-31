#include "vob.h"

#include <Tempest/Log>
#include <Tempest/Vec>

#include "world/objects/fireplace.h"
#include "world/objects/interactive.h"
#include "world/objects/staticobj.h"
#include "world/triggers/earthquake.h"
#include "world/triggers/movetrigger.h"
#include "world/triggers/movercontroler.h"
#include "world/triggers/codemaster.h"
#include "world/triggers/triggerlist.h"
#include "world/triggers/triggerscript.h"
#include "world/triggers/triggerworldstart.h"
#include "world/triggers/zonetrigger.h"
#include "world/triggers/messagefilter.h"
#include "world/triggers/pfxcontroller.h"
#include "world/triggers/trigger.h"
#include "world/triggers/touchdamage.h"
#include "world/triggers/cscamera.h"
#include "world/voblight.h"
#include "world/voblayer.h"
#include "world/world.h"
#include "game/serialize.h"

using namespace Tempest;

Vob::Vob(World& owner)
  : world(owner) {
  }

Vob::Vob(Vob* parent, World& owner, const asset::VirtualObject& vob, Flags flags)
  : world(owner), vobType(vob.type), vobObjectID(vob.id),
    layerObject(VobLayer::isAdded(vob)), parent(parent) {
  pos   = Tempest::Matrix4x4(vob.rotation.columns[0].x, vob.rotation.columns[1].x, vob.rotation.columns[2].x, vob.position.x,
                             vob.rotation.columns[0].y, vob.rotation.columns[1].y, vob.rotation.columns[2].y, vob.position.y,
                             vob.rotation.columns[0].z, vob.rotation.columns[1].z, vob.rotation.columns[2].z, vob.position.z,
                             0, 0, 0, 1);
  local = pos;

  if(parent!=nullptr) {
    auto m = parent->transform();
    m.inverse();
    m.mul(local);
    local = m;
    }

  for(auto& i:vob.children) {
    auto p = Vob::load(this,owner,*i,flags);
    if(p!=nullptr)
      child.emplace_back(std::move(p));
    }
  }

Vob::~Vob() {
  }

Vec3 Vob::position() const {
  const auto& p = transform();
  return Vec3(p.at(3,0),p.at(3,1),p.at(3,2));
  }

void Vob::setGlobalTransform(const Matrix4x4& p) {
  hasPhysicsPos = false;
  pos   = p;
  local = pos;

  if(parent!=nullptr) {
    auto m = parent->transform();
    m.inverse();
    m.mul(local);
    local = m;
    }
  }

void Vob::setLocalTransform(const Matrix4x4& p) {
  local = p;
  recalculateTransform();
  }

bool Vob::setMobState(std::string_view scheme, int32_t st) {
  bool ret = true;
  for(auto& i:child)
    ret &= i->setMobState(scheme,st);
  return ret;
  }

void Vob::moveEvent() {
  }

void Vob::physicsMoveEvent() {
  moveEvent();
  }

bool Vob::hasDynamicBody() const {
  return false;
  }

void Vob::setPhysicsTransform(const Matrix4x4& p) {
  // Simulation never rewrites the authored/save pose or relative placement.
  physicsPos = p;
  hasPhysicsPos = true;
  physicsMoveEvent();
  for(auto& i:child) {
    if(i->hasDynamicBody())
      continue;
    auto m = p;
    m.mul(i->local);
    i->setPhysicsTransform(m);
    }
  }

void Vob::fillAttachmentSnapshot(AttachmentSnapshot& out) const {
  out.zenId = vobObjectID;
  out.parentZenId = parent!=nullptr ? parent->vobObjectID : uint32_t(-1);
  out.transform = transform();
  }

void Vob::collectAttachmentSnapshots(std::vector<AttachmentSnapshot>& out) const {
  AttachmentSnapshot snapshot;
  fillAttachmentSnapshot(snapshot);
  out.emplace_back(std::move(snapshot));
  for(auto& i:child)
    i->collectAttachmentSnapshots(out);
  }

bool Vob::attachmentSnapshot(uint32_t zenId, std::vector<AttachmentSnapshot>& out) const {
  if(vobObjectID==zenId) {
    collectAttachmentSnapshots(out);
    return true;
    }
  for(auto& i:child)
    if(i->attachmentSnapshot(zenId,out))
      return true;
  return false;
  }

bool Vob::isDynamic() const {
  return false;
  }

float Vob::extendedSearchRadius() const {
  return 0;
  }

void Vob::recalculateTransform() {
  auto old = position();
  hasPhysicsPos = false;
  if(parent!=nullptr) {
    pos = parent->transform();
    pos.mul(local);
    } else {
    pos = local;
    }
  if(old!=position() && !isDynamic()) {
    switch(vobType) {
      case asset::VirtualObjectType::oCMOB:
      case asset::VirtualObjectType::oCMobBed:
      case asset::VirtualObjectType::oCMobDoor:
      case asset::VirtualObjectType::oCMobInter:
      case asset::VirtualObjectType::oCMobContainer:
      case asset::VirtualObjectType::oCMobSwitch:
      case asset::VirtualObjectType::oCMobLadder:
      case asset::VirtualObjectType::oCMobWheel:
        world.invalidateVobIndex();
        break;
      default:
        break;
      }
    }
  moveEvent();
  for(auto& i:child) {
    i->recalculateTransform();
    }
  }


std::unique_ptr<Vob> Vob::load(Vob* parent, World& world, const asset::VirtualObject& vob, Flags flags) {
  switch(vob.type) {
    case asset::VirtualObjectType::UNKNOWN:
      return nullptr;
    case asset::VirtualObjectType::zCVob:
    case asset::VirtualObjectType::zCVobStair:
      return std::unique_ptr<Vob>(new StaticObj(parent,world,vob,flags));
    case asset::VirtualObjectType::zCVobAnimate:
      // NOTE: engine animates all objects anyway
      return std::unique_ptr<Vob>(new StaticObj(parent,world,vob,flags));
    case asset::VirtualObjectType::zCVobLevelCompo:
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
    case asset::VirtualObjectType::oCMobFire:
      return std::unique_ptr<Vob>(new FirePlace(parent,world,reinterpret_cast<const asset::VFire&>(vob),flags));
    case asset::VirtualObjectType::oCMOB:
      // Irdotar bow-triggers
      // focusOverride=true
      return std::unique_ptr<Vob>(new Interactive(parent,world,reinterpret_cast<const asset::VMovableObject&>(vob),flags));
    case asset::VirtualObjectType::oCMobInter:
    case asset::VirtualObjectType::oCMobBed:
    case asset::VirtualObjectType::oCMobDoor:
    case asset::VirtualObjectType::oCMobContainer:
    case asset::VirtualObjectType::oCMobSwitch:
    case asset::VirtualObjectType::oCMobLadder:
    case asset::VirtualObjectType::oCMobWheel:
      return std::unique_ptr<Vob>(new Interactive(parent,world,reinterpret_cast<const asset::VMovableObject&>(vob),flags));

    case asset::VirtualObjectType::zCMover:
      return std::unique_ptr<Vob>(new MoveTrigger(parent,world,reinterpret_cast<const asset::VMover&>(vob),flags));
    case asset::VirtualObjectType::zCCodeMaster:
      return std::unique_ptr<Vob>(new CodeMaster(parent,world,reinterpret_cast<const asset::VCodeMaster&>(vob),flags));
    case asset::VirtualObjectType::zCTriggerList:
      return std::unique_ptr<Vob>(new TriggerList(parent,world,reinterpret_cast<const asset::VTriggerList&>(vob),flags));
    case asset::VirtualObjectType::oCTriggerScript:
      return std::unique_ptr<Vob>(new TriggerScript(parent,world,reinterpret_cast<const asset::VTriggerScript&>(vob),flags));
    case asset::VirtualObjectType::zCTriggerWorldStart:
      return std::unique_ptr<Vob>(new TriggerWorldStart(parent,world,reinterpret_cast<const asset::VTriggerWorldStart&>(vob),flags));
    case asset::VirtualObjectType::oCTriggerChangeLevel:
      return std::unique_ptr<Vob>(new ZoneTrigger(parent,world,reinterpret_cast<const asset::VTriggerChangeLevel&>(vob),flags));
    case asset::VirtualObjectType::zCTrigger:
      return std::unique_ptr<Vob>(new Trigger(parent,world,vob,flags));
    case asset::VirtualObjectType::zCMessageFilter:
      return std::unique_ptr<Vob>(new MessageFilter(parent,world,reinterpret_cast<const asset::VMessageFilter&>(vob),flags));
    case asset::VirtualObjectType::zCPFXController:
      return std::unique_ptr<Vob>(new PfxController(parent,world,reinterpret_cast<const asset::VParticleEffectController&>(vob),flags));
    case asset::VirtualObjectType::oCTouchDamage:
      return std::unique_ptr<Vob>(new TouchDamage(parent,world,reinterpret_cast<const asset::VTouchDamage&>(vob),flags));
    case asset::VirtualObjectType::zCTriggerUntouch:
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
    case asset::VirtualObjectType::zCMoverController:
      return std::unique_ptr<Vob>(new MoverControler(parent,world,reinterpret_cast<const asset::VMoverController&>(vob),flags));
    case asset::VirtualObjectType::zCEarthquake:
      return std::unique_ptr<Vob>(new Earthquake(parent,world,reinterpret_cast<const asset::VEarthquake&>(vob),flags));
    case asset::VirtualObjectType::zCVobStartpoint: {
      float dx = vob.rotation[2].x;
      float dy = vob.rotation[2].y;
      float dz = vob.rotation[2].z;
      world.addStartPoint(Vec3(vob.position.x,vob.position.y,vob.position.z),Vec3(dx,dy,dz),vob.vob_name);
      // FIXME
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
      }
    case asset::VirtualObjectType::zCVobSpot: {
      float dx = vob.rotation[2].x;
      float dy = vob.rotation[2].y;
      float dz = vob.rotation[2].z;
      world.addFreePoint(Vec3(vob.position.x,vob.position.y,vob.position.z),Vec3(dx,dy,dz),vob.vob_name);
      // FIXME
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
      }
    case asset::VirtualObjectType::oCItem: {
      if(flags)
        world.addItem(reinterpret_cast<const asset::VItem&>(vob));
      // FIXME
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
      }

    case asset::VirtualObjectType::zCVobSound:
    case asset::VirtualObjectType::zCVobSoundDaytime:
    case asset::VirtualObjectType::oCZoneMusic:
    case asset::VirtualObjectType::oCZoneMusicDefault: {
      world.addSound(vob);
      // FIXME
      return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
      }
    case asset::VirtualObjectType::zCVobLight: {
      return std::unique_ptr<Vob>(new VobLight(parent,world,reinterpret_cast<const asset::VLight&>(vob),flags));
      }
    case asset::VirtualObjectType::zCCSCamera:
      return std::unique_ptr<Vob>(new CsCamera(parent,world,reinterpret_cast<const asset::VCutsceneCamera&>(vob),flags));
    case asset::VirtualObjectType::zCCamTrj_KeyFrame:
      break;
    case asset::VirtualObjectType::zCZoneZFog:
    case asset::VirtualObjectType::zCZoneZFogDefault:
      break;
    case asset::VirtualObjectType::zCZoneVobFarPlane:
    case asset::VirtualObjectType::zCZoneVobFarPlaneDefault:
      // wont do: no distance culling in any plans
      break;
    case asset::VirtualObjectType::zCVobLensFlare:
    case asset::VirtualObjectType::zCVobScreenFX:
      break;
    case asset::VirtualObjectType::oCNpc:
    case asset::VirtualObjectType::oCCSTrigger:
      break;
    }

  return std::unique_ptr<Vob>(new Vob(parent,world,vob,flags));
  }

void Vob::saveVobTree(Serialize& fin) const {
  for(auto& i:child)
    i->saveVobTree(fin);
  // Layer additions are static content, reconstructed once from the current
  // world on every load, and never enter the base-ID save namespace.
  if(layerObject || vobType==asset::VirtualObjectType::zCVob)
    return;
  if(vobObjectID!=uint32_t(-1))
    save(fin);
  }

void Vob::loadVobTree(Serialize& fin) {
  if(fin.version()<43) {
    if(vobType==asset::VirtualObjectType::zCEarthquake ||
       vobType==asset::VirtualObjectType::zCCSCamera)
      return;
    }

  for(auto& i:child)
    i->loadVobTree(fin);
  if(!layerObject && vobObjectID!=uint32_t(-1) && vobType!=asset::VirtualObjectType::zCVob)
    load(fin);
  }

void Vob::save(Serialize& fout) const {
  fout.setEntry("worlds/",fout.worldName(),"/mobsi/",vobObjectID,"/data");
  fout.write(uint8_t(vobType),pos,local);
  }

void Vob::load(Serialize& fin) {
  if(!fin.setEntry("worlds/",fin.worldName(),"/mobsi/",vobObjectID,"/data"))
    return;
  auto type = uint8_t(vobType);
  uint8_t savValue;
  fin.read(savValue,pos,local);
  hasPhysicsPos = false;

  if(fin.version() > 41) {
    if(savValue!=type)
      throw std::logic_error("inconsistent *.sav vs world");
    }
  moveEvent();
  }
