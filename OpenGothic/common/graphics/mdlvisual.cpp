#include "mdlvisual.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "game/serialize.h"
#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "objvisual.h"
#include "gothic.h"
#include "graphics/mesh/animmath.h"
#include "graphics/mesh/ikmath.h"
#include "frameprofile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using namespace Tempest;

namespace {

constexpr uint32_t ArrowSaveMagic=0x31525041u; // APR1, independent of the NPC/visual layout.
constexpr uint16_t ArrowSaveVersion=1;
constexpr size_t MaxArrowSaveName=512;

bool readArrowName(Serialize& fin, std::string& value) {
  uint32_t size=0;
  fin.read(size);
  if(size==0 || size>MaxArrowSaveName)
    return false;
  value.resize(size);
  fin.readBytes(value.data(),size);
  return value.find('\0')==std::string::npos;
  }

bool inverseBasis(const Matrix4x4& m, double inverse[3][3]) {
  const double a=m.at(0,0), b=m.at(1,0), c=m.at(2,0);
  const double d=m.at(0,1), e=m.at(1,1), f=m.at(2,1);
  const double g=m.at(0,2), h=m.at(1,2), i=m.at(2,2);
  const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
  if(!std::isfinite(det) || std::abs(det)<1e-12)
    return false;
  inverse[0][0]=(e*i-f*h)/det; inverse[0][1]=(c*h-b*i)/det; inverse[0][2]=(b*f-c*e)/det;
  inverse[1][0]=(f*g-d*i)/det; inverse[1][1]=(a*i-c*g)/det; inverse[1][2]=(c*d-a*f)/det;
  inverse[2][0]=(d*h-e*g)/det; inverse[2][1]=(b*g-a*h)/det; inverse[2][2]=(a*e-b*d)/det;
  return true;
  }

bool validRenderFrame(const Matrix4x4& m) {
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      if(!std::isfinite(m.at(c,r)))
        return false;
  return std::abs(m.at(0,3))<1e-5f && std::abs(m.at(1,3))<1e-5f &&
         std::abs(m.at(2,3))<1e-5f && std::abs(m.at(3,3)-1.f)<1e-5f;
  }

bool blendPhysicalBone(const Matrix4x4& animated, const Matrix4x4& physical, float weight, Matrix4x4& out) {
  if(weight>=1.f) {
    out=physical;
    return true;
    }
  double inverse[3][3];
  if(!inverseBasis(animated,inverse))
    return false;
  // Native frames retain the animation's affine stretch. Their world-space delta is rigid;
  // interpolate that rotation, then apply it to the intact animated basis.
  auto delta=Matrix4x4::mkIdentity();
  for(int c=0; c<3; ++c)
    for(int r=0; r<3; ++r) {
      double value=0;
      for(int k=0; k<3; ++k)
        value+=double(physical.at(k,r))*inverse[k][c];
      delta.set(c,r,float(value));
      }
  const auto rotation=IkMath::toMat3(IkMath::slerp({},IkMath::toQuat(IkMath::rotationOf(delta)),weight));
  out=animated;
  for(int c=0; c<3; ++c)
    for(int r=0; r<3; ++r) {
      float value=0;
      for(int k=0; k<3; ++k)
        value+=rotation.m[r][k]*animated.at(c,k);
      out.set(c,r,value);
      }
  for(int r=0; r<3; ++r)
    out.set(3,r,float(double(animated.at(3,r))+(double(physical.at(3,r))-animated.at(3,r))*weight));
  return true;
  }

bool followPhysicalParent(const Matrix4x4& animatedParent, const Matrix4x4& renderedParent,
                          const Matrix4x4& animated, Matrix4x4& out) {
  double inverse[3][3];
  if(!inverseBasis(animatedParent,inverse))
    return false;
  auto local=Matrix4x4::mkIdentity();
  for(int c=0; c<4; ++c)
    for(int r=0; r<3; ++r) {
      double value=0;
      for(int k=0; k<3; ++k) {
        const double v=c<3 ? double(animated.at(c,k)) : double(animated.at(3,k))-animatedParent.at(3,k);
        value+=inverse[r][k]*v;
        }
      local.set(c,r,float(value));
      }
  // Subtract world translations before inversion, keeping small slot/finger offsets precise.
  out=renderedParent*local;
  return true;
  }

}

MdlVisual::MdlVisual()
  :skInst(std::make_unique<Pose>()) {
  pos.identity();
  }

MdlVisual::~MdlVisual() {
  }

void MdlVisual::save(Serialize &fout, const Npc&) const {
  fout.write(fgtMode);
  if(skeleton!=nullptr)
    fout.write(skeleton->name()); else
    fout.write(std::string(""));
  solver.save(fout);
  skInst->save(fout);
  }

void MdlVisual::save(Serialize& fout, const Interactive&) const {
  solver.save(fout);
  skInst->save(fout);
  }

void MdlVisual::load(Serialize& fin, Npc& npc) {
  clearStuckArrows();
  clearPhysicalPose();
  std::string s;

  fin.read(fgtMode);
  fin.read(s);
  npc.setVisual(s);

  solver.load(fin);
  skInst->load(fin,solver);
  if(skeleton!=nullptr)
    rebindAttaches(*skeleton);
  }

void MdlVisual::load(Serialize& fin, Interactive&) {
  clearStuckArrows();
  clearPhysicalPose();
  solver.load(fin);
  skInst->load(fin,solver);
  if(skeleton!=nullptr)
    rebindAttaches(*skeleton);
  }

// Mdl_SetVisual
void MdlVisual::setVisual(const Skeleton *v) {
  if(v!=skeleton)
    clearStuckArrows();
  clearPhysicalPose();
  if(v!=nullptr)
    rebindAttaches(*v);
  solver.setSkeleton(v);
  skInst->setSkeleton(v);
  view.setSkeleton(v);

  skeleton = v;
  hitZones.setSkeleton(v);
  setObjMatrix(pos); // invalidate obj matrix
  }

bool MdlVisual::setPhysicalPose(const std::vector<Matrix4x4>& physical,
                                const std::vector<size_t>& bones, float weight) {
  const size_t count=skInst->boneCount();
  if(skeleton==nullptr || skeleton->nodes.size()!=count || physical.size()!=count || bones.empty() ||
     !std::isfinite(weight) || weight<=0.f)
    return clearPhysicalPose();
  for(const auto i:bones)
    if(i>=count || !validRenderFrame(physical[i]))
      return clearPhysicalPose();

  const auto* animated=skInst->transform();
  renderedPose.assign(animated,animated+count);
  renderedMask.assign(count,0);
  constexpr uint8_t Selected=1, Finished=2, Affected=4, Visiting=8;
  for(const auto i:bones)
    renderedMask[i]=Selected;
  weight=std::min(weight,1.f);
  auto bone=[&](auto&& self, size_t i) -> bool {
    if(renderedMask[i]&Finished)
      return true;
    if(renderedMask[i]&Visiting)
      return false;
    renderedMask[i]|=Visiting;
    const size_t parent=skeleton->nodes[i].parent;
    if(parent<count && !self(self,parent))
      return false;
    if(renderedMask[i]&Selected) {
      if(!blendPhysicalBone(animated[i],physical[i],weight,renderedPose[i]))
        return false;
      renderedMask[i]|=Affected;
      }
    else if(parent<count && (renderedMask[parent]&Affected)) {
      if(!followPhysicalParent(animated[parent],renderedPose[parent],animated[i],renderedPose[i]))
        return false;
      renderedMask[i]|=Affected;
      }
    renderedMask[i]=uint8_t((renderedMask[i]|Finished)&~Visiting);
    return true;
    };
  for(size_t i=0; i<count; ++i)
    if(!bone(bone,i))
      return clearPhysicalPose();
  view.setPose(pos,renderedPose.data());
  return true;
  }

bool MdlVisual::clearPhysicalPose() {
  if(renderedPose.empty())
    return false;
  renderedPose.clear();
  renderedMask.clear();
  view.setPose(pos,*skInst);
  return true;
  }

const Matrix4x4& MdlVisual::renderedBone(size_t bone) const {
  if(bone<renderedPose.size())
    return renderedPose[bone];
  return skInst->bone(bone);
  }

bool MdlVisual::stickArrow(MeshObjects::Mesh&& arrow, size_t bone, const Matrix4x4& worldMatrix,
                           std::string_view visualName) {
  if(arrow.isEmpty() || skeleton==nullptr || bone>=skeleton->nodes.size() || bone>=skInst->boneCount() ||
     !validRenderFrame(worldMatrix))
    return false;
  const auto& animated=skInst->bone(bone);
  double inverse[3][3];
  if(!validRenderFrame(animated) || !inverseBasis(animated,inverse))
    return false;
  ArrowPin pin;
  pin.visual=visualName;
  pin.bone=bone;
  pin.local.identity();
  // Keep the complete arrow basis, including scale. Subtract world translations first so
  // an impact far from the origin retains its small offset from the animated bone.
  for(int c=0; c<4; ++c)
    for(int r=0; r<3; ++r) {
      double value=0;
      for(int k=0; k<3; ++k) {
        const double v=c<3 ? double(worldMatrix.at(c,k)) : double(worldMatrix.at(3,k))-animated.at(3,k);
        value+=inverse[r][k]*v;
        }
      pin.local.set(c,r,float(value));
      }
  if(!validRenderFrame(pin.local))
    return false;
  arrow.setObjMatrix(renderedBone(bone)*pin.local);
  pin.view=std::move(arrow);
  if(stuckArrows.size()>=MaxStuckArrows)
    stuckArrows.erase(stuckArrows.begin());
  stuckArrows.push_back(std::move(pin));
  return true;
  }

void MdlVisual::saveStuckArrows(Serialize& fout) const {
  fout.write(ArrowSaveMagic,ArrowSaveVersion);
  fout.write(skeleton!=nullptr ? skeleton->name() : std::string_view());
  uint32_t count=0;
  for(const auto& pin:stuckArrows)
    if(!pin.visual.empty() && skeleton!=nullptr && pin.bone<skeleton->nodes.size())
      ++count;
  fout.write(count);
  for(const auto& pin:stuckArrows) {
    if(pin.visual.empty() || skeleton==nullptr || pin.bone>=skeleton->nodes.size())
      continue;
    fout.write(pin.visual,skeleton->nodes[pin.bone].name,pin.local);
    }
  }

bool MdlVisual::loadStuckArrows(Serialize& fin, World& world) {
  clearStuckArrows();
  if(!Gothic::physicsFlags().arrowSticking)
    return true;
  try {
    uint32_t magic=0,count=0;
    uint16_t version=0;
    std::string savedSkeleton;
    fin.read(magic,version);
    if(magic!=ArrowSaveMagic || version!=ArrowSaveVersion ||
       !readArrowName(fin,savedSkeleton) || skeleton==nullptr || savedSkeleton!=skeleton->name())
      return false;
    fin.read(count);
    if(count>MaxStuckArrows)
      return false;
    std::vector<ArrowPin> loaded;
    loaded.reserve(count);
    for(uint32_t i=0; i<count; ++i) {
      ArrowPin pin;
      std::string boneName;
      if(!readArrowName(fin,pin.visual) || !readArrowName(fin,boneName))
        return false;
      fin.read(pin.local);
      pin.bone=skeleton->findNode(boneName);
      double inverse[3][3];
      if(pin.bone>=skeleton->nodes.size() || pin.bone>=skInst->boneCount() ||
         !validRenderFrame(pin.local) || !inverseBasis(pin.local,inverse))
        return false;
      for(int c=0; c<4; ++c)
        for(int r=0; r<3; ++r)
          if(std::abs(pin.local.at(c,r))>100000000.f)
            return false;
      pin.view=world.addView(pin.visual);
      if(pin.view.isEmpty())
        return false;
      pin.view.setObjMatrix(renderedBone(pin.bone)*pin.local);
      loaded.push_back(std::move(pin));
      }
    stuckArrows=std::move(loaded);
    return true;
    } catch(const std::exception&) {
    return false;
    }
  }

void MdlVisual::clearStuckArrows() {
  stuckArrows.clear();
  }

bool MdlVisual::stuckArrowProbe(size_t index, ArrowPinProbe& out) const {
  if(index>=stuckArrows.size())
    return false;
  const auto& pin=stuckArrows[index];
  if(pin.view.isEmpty())
    return false;
  out.bone=pin.bone;
  out.local=pin.local;
  out.world=pin.view.node(0).position();
  return true;
  }

void MdlVisual::setYTranslationEnable(bool e) {
  if(e)
    skInst->setFlags(Pose::NoFlags); else
    skInst->setFlags(Pose::NoTranslation);
  }

void MdlVisual::setVisualBody(World& owner, MeshObjects::Mesh&& body) {
  implSetBody(nullptr,owner,std::move(body),0);
  }

// Mdl_SetVisualBody
void MdlVisual::setVisualBody(Npc& npc, MeshObjects::Mesh &&h, MeshObjects::Mesh &&body, int32_t version) {
  bind(head,std::move(h),"BIP01 HEAD");
  implSetBody(&npc,npc.world(),std::move(body),version);
  head.view.setAsGhost(hnpcFlagGhost);
  head.view.setFatness(npc.fatness());
  }

bool MdlVisual::hasOverlay(const Skeleton* sk) const {
  return solver.hasOverlay(sk);
  }

// Mdl_ApplyOverlayMdsTimed, Mdl_ApplyOverlayMds
void MdlVisual::addOverlay(const Skeleton *sk, uint64_t time) {
  solver.addOverlay(sk,time);
  }

// Mdl_RemoveOverlayMDS
void MdlVisual::delOverlay(std::string_view sk) {
  solver.delOverlay(sk);
  }

// Mdl_RemoveOverlayMDS
void MdlVisual::delOverlay(const Skeleton *sk) {
  solver.delOverlay(sk);
  }

void MdlVisual::clearOverlays() {
  solver.clearOverlays();
  }

void MdlVisual::setBody(Npc& npc, MeshObjects::Mesh&& a, const int32_t version) {
  implSetBody(&npc,npc.world(),std::move(a),version);
  setObjMatrix(pos);
  }

void MdlVisual::setArmor(Npc& npc, MeshObjects::Mesh&& armor) {
  // NOTE: Giant_Bug have no version tag in attachments;
  // Light dragon hunter armour has broken attachment with no tags
  // Big dragon hunter armour has many atachments with version tags
  implSetBody(&npc,npc.world(),std::move(armor),-1);
  setObjMatrix(pos);
  }

void MdlVisual::implSetBody(Npc* npc, World& world, MeshObjects::Mesh&& body, const int32_t version) {
  clearPhysicalPose();
  attach.clear();
  if(auto p = body.protoMesh()) {
    for(auto& att:p->attach) {
      if(!att.hasNode) {
        auto view = world.addAtachView(att,version);
        setSlotAttachment(std::move(view),att.name);
        }
      }
    syncAttaches();
    }
  view = std::move(body);
  view.setAsGhost(hnpcFlagGhost);
  view.setFatness(npc==nullptr ? 0 : npc->fatness());
  view.setSkeleton(skeleton);
  view.setPose(pos,*skInst);
  hnpcVisual.view.setMesh(&view);
  fitHitZones();
  }

void MdlVisual::fitHitZones() {
  // the zones as thick as the body is: its mesh by bone, its parts on the bones of their names, the head's
  // mesh on its bone
  std::vector<HitZones::Shape> shape;
  if(auto p = view.protoMesh()) {
    shape.push_back({&p->nodePoints,size_t(-1),nullptr});
    for(size_t i=0; skeleton!=nullptr && i<p->attach.size() && i<p->attachPoints.size(); ++i)
      if(!p->attach[i].hasNode)
        shape.push_back({nullptr,skeleton->findNode(p->attach[i].name),&p->attachPoints[i]});
    }
  if(auto p = head.view.protoMesh(); p!=nullptr && skeleton!=nullptr)
    shape.push_back({nullptr,skeleton->findNode("BIP01 HEAD"),&p->points});
  hitZones.fit(shape);
  }

void MdlVisual::setSlotAttachment(MeshObjects::Mesh&& itm, std::string_view bone) {
  if(bone.empty() || skeleton==nullptr)
    return;

  size_t id = skeleton->findNode(bone);
  if(id==size_t(-1))
    return;

  for(auto& i:attach) {
    if(i.boneId==id) {
      i.view=std::move(itm);
      syncAttaches();
      return;
      }
    }

  MeshAttach slt;
  slt.bone = skeleton->nodes[id].name;
  bind(slt,std::move(itm),slt.bone);
  attach.push_back(std::move(slt));
  }

void MdlVisual::setSword(MeshObjects::Mesh &&s) {
  bind(sword,std::move(s),"ZS_RIGHTHAND");
  }

void MdlVisual::setRangedWeapon(MeshObjects::Mesh &&b) {
  bind(bow,std::move(b),"ZS_BOW");
  }

void MdlVisual::setShield(MeshObjects::Mesh&& s) {
  bind(shield,std::move(s),"ZS_SHIELD");
  }

void MdlVisual::setAmmoItem(MeshObjects::Mesh&& a, std::string_view bone) {
  bind(ammunition,std::move(a),bone);
  }

void MdlVisual::dropAmmo() {
  ammunition.view = MeshObjects::Mesh();
  }

void MdlVisual::setMagicWeapon(Effect&& spell, World& owner) {
  auto n = std::move(pfx.view);
  n.setLooped(false);
  n.setActive(false);
  startEffect(owner,std::move(n),0,true);

  pfx.view = std::move(spell);
  pfx.view.setLooped(true);
  pfx.view.setPhysicsDisable();
  if(skeleton!=nullptr)
    pfx.view.bindAttaches(*skInst,*skeleton);
  }

void MdlVisual::setMagicWeaponKey(World& owner, SpellFxKey key, int32_t keyLvl) {
  pfx.view.setKey(owner,key,keyLvl);
  }

void MdlVisual::setSlotItem(MeshObjects::Mesh &&itm, std::string_view bone) {
  if(bone.empty() || skeleton==nullptr)
    return;

  size_t id = skeleton->findNode(bone);
  if(id==size_t(-1))
    return;

  // HACK: light dragon hunter armour
  if(id==head.boneId && !head.view.isEmpty())
    return;

  for(auto& i:item) {
    if(i.boneId==id) {
      i.view=std::move(itm);
      syncAttaches();
      return;
      }
    }

  MeshAttach slt;
  slt.bone = skeleton->nodes[id].name;
  bind(slt,std::move(itm),slt.bone);
  item.push_back(std::move(slt));
  syncAttaches();
  }

void MdlVisual::setStateItem(MeshObjects::Mesh&& a, std::string_view bone) {
  bind(stateItm,std::move(a),bone);
  syncAttaches();
  }

void MdlVisual::clearSlotItem(std::string_view bone) {
  size_t id = skeleton==nullptr ? size_t(-1) : skeleton->findNode(bone);

  for(size_t i=0;i<item.size();++i) {
    if(id==size_t(-1) || item[i].boneId==id) {
      item[i] = std::move(item.back());
      item.pop_back();
      syncAttaches();
      return;
      }
    }
  }

bool MdlVisual::setFightMode(asset::MdsFightMode mode) {
  WeaponState f = WeaponState::NoWeapon;

  switch(mode) {
    case asset::MdsFightMode::INVALID:
      return false;
    case asset::MdsFightMode::NONE:
      f=WeaponState::NoWeapon;
      break;
    case asset::MdsFightMode::FIST:
      f=WeaponState::Fist;
      break;
    case asset::MdsFightMode::SINGLE_HANDED:
      f=WeaponState::W1H;
      break;
    case asset::MdsFightMode::DUAL_HANDED:
      f=WeaponState::W2H;
      break;
    case asset::MdsFightMode::BOW:
      f=WeaponState::Bow;
      break;
    case asset::MdsFightMode::CROSSBOW:
      f=WeaponState::CBow;
      break;
    case asset::MdsFightMode::MAGIC:
      f=WeaponState::Mage;
      break;
    }

  return setToFightMode(f);
  }

void MdlVisual::dropWeapon(Npc& npc) {
  MeshAttach* att  = &sword;
  auto&       pose = *skInst;

  if(fgtMode!=WeaponState::W1H && fgtMode!=WeaponState::W2H &&
     fgtMode!=WeaponState::Bow && fgtMode!=WeaponState::CBow)
    return;

  auto p = pos;
  if(att->boneId<pose.boneCount())
    p = pose.bone(att->boneId);

  Item* itm = nullptr;
  if(fgtMode==WeaponState::W1H || fgtMode==WeaponState::W2H)
    itm = npc.currentMeleeWeapon(); else
    itm = npc.currentRangedWeapon();

  if(itm==nullptr)
    return;

  auto it = npc.world().addItemDyn(itm->clsId(),p,npc.handle().symbol_index());
  it->setCount(1);

  npc.delItem(itm->clsId(),1);
  }

void MdlVisual::dropShield(Npc& npc) {
  MeshAttach* att  = &sword;
  auto&       pose = *skInst;

  if(fgtMode!=WeaponState::W1H && fgtMode!=WeaponState::W2H)
    return;

  auto p = pos;
  if(att->boneId<pose.boneCount())
    p = pose.bone(att->boneId);

  Item* itm = npc.currentShield();
  if(itm==nullptr)
    return;

  auto it = npc.world().addItemDyn(itm->clsId(),p,npc.handle().symbol_index());
  it->setCount(1);

  npc.delItem(itm->clsId(),1);
  }

void MdlVisual::startEffect(World& owner, Effect&& vfx, int32_t slot, bool noSlot) {
  uint64_t timeUntil = vfx.effectPrefferedTime();
  if(timeUntil!=0)
    timeUntil+=owner.tickCount();

  if(skeleton==nullptr)
    return;

  vfx.setMesh(&view);
  vfx.setBullet(nullptr,owner);

  for(auto& i:effects) {
    if(i.id==slot && !i.noSlot) {
      i.timeUntil = timeUntil;
      i.view = std::move(vfx);
      i.view.bindAttaches(*skInst,*skeleton);
      syncAttaches();
      return;
      }
    }

  PfxSlot slt;
  slt.id        = slot;
  slt.timeUntil = timeUntil;
  slt.noSlot    = noSlot;
  slt.view      = std::move(vfx);
  slt.view.bindAttaches(*skInst,*skeleton);
  effects.push_back(std::move(slt));
  syncAttaches();
  }

void MdlVisual::stopEffect(const VisualFx& vfx) {
  for(size_t i=0;i<effects.size();++i) {
    if(effects[i].view.is(vfx)) {
      effects[i] = std::move(effects.back());
      effects.pop_back();
      syncAttaches();
      return;
      }
    }
  }

void MdlVisual::stopEffect(int32_t slot) {
  for(size_t i=0;i<effects.size();++i) {
    if(effects[i].id==slot) {
      effects[i] = std::move(effects.back());
      effects.pop_back();
      syncAttaches();
      return;
      }
    }
  }

void MdlVisual::setNpcEffect(World& owner, Npc& npc, std::string_view s, zenkit::NpcFlag flags) {
  if(hnpcVisualName!=s) {
    hnpcVisualName = s;
    auto vfx = Gothic::inst().loadVisualFx(s);
    if(vfx==nullptr) {
      hnpcVisual.view = Effect();
      return;
      }
    hnpcVisual.view = Effect(*vfx,owner,npc,SpellFxKey::Count);
    if(skeleton!=nullptr)
      hnpcVisual.view.bindAttaches(*skInst,*skeleton);
    hnpcVisual.view.setActive(true);
    hnpcVisual.view.setLooped(true);
    hnpcVisual.view.setMesh(&view);
    }

  const bool nextGhost = (flags & zenkit::NpcFlag::GHOST);
  if(hnpcFlagGhost!=nextGhost) {
    hnpcFlagGhost=nextGhost;
    view.setAsGhost(hnpcFlagGhost);
    head.view.setAsGhost(hnpcFlagGhost);
    for(auto& i:attach)
      i.view.setAsGhost(hnpcFlagGhost);
    }
  }

void MdlVisual::setFatness(float fatness) {
  view.setFatness(fatness);
  head.view.setFatness(fatness);
  for(auto& i:attach)
    i.view.setFatness(fatness);
  }

void MdlVisual::emitBlockEffect(Npc& dest, Npc& source) {
  auto& world = source.world();

  auto& pose  = *skInst;
  auto  p     = pos;
  if(sword.boneId<pose.boneCount())
    p = pose.bone(sword.boneId);

  auto src = source.inventory().activeWeapon();
  auto dst = dest  .inventory().activeWeapon();

  if(src==nullptr || dst==nullptr)
    return;

  auto s = world.addWeaponBlkEffect(ItemMaterial(src->handle().material),ItemMaterial(dst->handle().material),p);
  s.play();
  }

bool MdlVisual::setToFightMode(const WeaponState f) {
  if(f==fgtMode)
    return false;
  fgtMode = f;
  return true;
  }

void MdlVisual::setObjMatrix(const Tempest::Matrix4x4 &m, bool syncAttach) {
  FrameProfile::Scope timing(FrameProfile::Zone::PaletteCpu,FrameProfile::actorContext());
  pos = m;
  skInst->setObjectMatrix(m,syncAttach);
  view.setPose(m,renderedPose.empty() ? skInst->transform() : renderedPose.data());
  if(syncAttach)
    syncAttaches();
  }

bool MdlVisual::minSkinnedVertex(float fatness,Vec3& point) const {
  const auto* mesh=view.protoMesh();
  if(mesh==nullptr)
    return false;
  const auto* bones=renderedPose.empty() ? skInst->transform() : renderedPose.data();
  return mesh->minSkinnedVertex(bones,skInst->boneCount(),pos,fatness,point);
  }

void MdlVisual::setHeadRotation(float dx, float dz) {
  skInst->setHeadRotation(dx,dz);
  syncAttaches(head);
  }

Vec2 MdlVisual::headRotation() const {
  return skInst->headRotation();
  }

void MdlVisual::updateWeaponSkeleton(const Item* weapon, const Item* range) {
  auto st = fgtMode;
  if(st==WeaponState::W1H || st==WeaponState::W2H){
    bind(sword, "ZS_RIGHTHAND");
    } else {
    bool twoHands = weapon!=nullptr && weapon->is2H();
    bind(sword,twoHands ? "ZS_LONGSWORD" : "ZS_SWORD");
    }

  if(st==WeaponState::Bow || st==WeaponState::CBow){
    if(st==WeaponState::Bow)
      bind(bow,"ZS_LEFTHAND"); else
      bind(bow,"ZS_RIGHTHAND");
    } else {
    bool cbow  = range!=nullptr && range->isCrossbow();
    bind(bow,cbow ? "ZS_CROSSBOW" : "ZS_BOW");
    }

  bind(shield, st==WeaponState::W1H ? "ZS_LEFTARM" : "ZS_SHIELD");

  pfx.view.setActive(st==WeaponState::Mage);
  syncAttaches();
  }

void MdlVisual::setTorch(bool t, World& owner) {
  if(!t) {
    torch.view.reset();
    return;
    }
  size_t torchId = owner.script().findSymbolIndex("ItLsTorchburning");
  if(torchId==size_t(-1))
    return;

  auto hitem = std::make_shared<zenkit::IItem>();
  owner.script().initializeInstanceItem(hitem, torchId);
  torch.view.reset(new ObjVisual());
  torch.view->setVisual(*hitem,owner,false);
  torch.boneId = (skeleton==nullptr ? size_t(-1) : skeleton->findNode("ZS_LEFTHAND"));

  auto& pose = *skInst;
  auto  p    = pos;
  if(torch.boneId<pose.boneCount())
    p = renderedBone(torch.boneId);
  torch.view->setObjMatrix(p);
  }

bool MdlVisual::isUsingTorch() const {
  return torch.view!=nullptr;
  }

bool MdlVisual::updateAnimation(Npc* npc, Interactive* mobsi, World& world, uint64_t dt, bool force, bool pausePose) {
  Pose&    pose      = *skInst;
  uint64_t tickCount = world.tickCount();
  auto     pos3      = Vec3{pos.at(3,0), pos.at(3,1), pos.at(3,2)};

  if(!pausePose && npc!=nullptr && world.isInSfxRange(pos3))
    pose.processSfx(*npc,tickCount);
  if(!pausePose && mobsi!=nullptr && world.isInSfxRange(pos3))
    pose.processSfx(*mobsi,tickCount);
  if(!pausePose && world.isInPfxRange(pos3))
    pose.processPfx(*this,world,tickCount);

  for(size_t i=0;i<effects.size();) {
    if(effects[i].timeUntil<tickCount) {
      effects[i] = std::move(effects.back());
      effects.pop_back();
      } else {
      effects[i].view.tick(dt);
      ++i;
      }
    }

  // Effects retain their world-clock lifetime while the actor's solved bones hold still.
  if(pausePose)
    return false;

  solver.update(tickCount);
  pose.setObjectMatrix(pos,false);
  const bool changed=[&] { FrameProfile::Scope timing(FrameProfile::Zone::PoseCpu,FrameProfile::watchedActor(npc));
    return pose.update(tickCount, force); }();
  if(npc!=nullptr && npc->isPlayer()) {
    static const bool log = std::getenv("OPENGOTHIC_ANIBLEND_LOG")!=nullptr;
    pose.setBlendLog(log);
    }
  // the animated pose for the test bench (it probes the feet too)
  if(npc!=nullptr && npc->isPlayer() && FootIk::isProbing())
    pose.probe(Pose::playerProbe);
  if(npc!=nullptr && npc==Pose::probeNpc)
    pose.probe(Pose::subjectProbe);

  if(changed) {
    FrameProfile::Scope timing(FrameProfile::Zone::PaletteCpu,FrameProfile::watchedActor(npc));
    view.setPose(pos,pose);
    }
  return changed;
  }

bool MdlVisual::updateGaze(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& gaze = pose.gazeIk();
  if(!gaze.isValid()) {
    if(&npc==Gaze::probeNpc)
      Gaze::subjectProbe = Gaze::Probe();
    return false;
    }

  const float a = npc.rotationRad();
  Gaze::Input in;
  in.dt      = dt;
  in.forward = {std::cos(a), 0, std::sin(a)};
  in.talk    = {gestures.head().pitch, gestures.head().yaw, gestures.head().roll};
  // a blow throws the head around: the gaze lets go while one stumbles
  if(!npc.isDown() && (npc.bodyStateMasked() & BS_MAX)!=BS_STUMBLE)
    in.source = npc.gazeTarget(in.target);
  // thinking of an answer: the eyes go aside and up from the one it talks to (TalkGestures)
  if(const auto& as = gestures.eyesAside(); as.weight>0.f && in.source==Gaze::Source::Talk) {
    const Vec3  head = npc.mapHeadBone();
    const Vec3  d    = in.target - head;
    const float len  = std::sqrt(d.x*d.x + d.z*d.z);
    if(len>1.f) {
      const float yaw = as.yaw*as.weight;
      const float c = std::cos(yaw), s = std::sin(yaw);
      // around Up, + to the left as Gaze turns
      const Vec3 r = {d.x*c - d.z*s, 0, d.x*s + d.z*c};
      in.target = head + Vec3(r.x, d.y + len*std::tan(as.pitch*as.weight), r.z);
      }
    }
  if(in.source!=Gaze::Source::None) {
    in.active   = true;
    // the hero's body is the player's; one walking or busy at a bench, a book turns the head only;
    // a four-legged spine does not turn like a man's
    const auto bs = npc.bodyStateMasked() & BS_MAX;
    // aiming by hand the trunk is the aim's: a turn of it would throw the arrow off
    in.headOnly = in.source==Gaze::Source::Input || npc.isPlayer() || npc.hasManualAim() || npc.interactive()!=nullptr || pose.footIk().isQuadruped() ||
                  bs==(BS_WALK & BS_MAX) || bs==BS_RUN || bs==BS_SPRINT || bs==(BS_SNEAK & BS_MAX);
    }
  const bool changed = pose.updateGaze(in);
  if(npc.isPlayer())
    gaze.probe(Gaze::playerProbe);
  if(&npc==Gaze::probeNpc)
    gaze.probe(Gaze::subjectProbe);
  if(changed)
    view.setPose(pos,pose);
  return changed;
  }

bool MdlVisual::updateLean(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& lean = pose.leanIk();
  if(!lean.isValid()) {
    if(npc.isPlayer())
      Lean::playerProbe = Lean::Probe();
    if(&npc==Lean::probeNpc)
      Lean::subjectProbe = Lean::Probe();
    return false;
    }

  // on the ground, going somewhere; four legs turn and climb their own way (FootIk)
  const float a  = npc.rotationRad();
  const auto  bs = npc.bodyStateMasked() & BS_MAX;
  Lean::Input in;
  in.dt       = dt;
  in.position = npc.position();
  in.forward  = {std::cos(a), 0, std::sin(a)};
  in.active   = (bs==(BS_WALK & BS_MAX) || bs==BS_RUN || bs==BS_SPRINT || bs==(BS_SNEAK & BS_MAX)) &&
                !npc.isDown() && !npc.isInAir() && !npc.isSwim() && !npc.isDive() && !npc.isSlide() &&
                npc.interactive()==nullptr && !pose.footIk().isQuadruped();
  const bool changed = pose.updateLean(in);
  if(npc.isPlayer())
    lean.probe(Lean::playerProbe);
  if(&npc==Lean::probeNpc)
    lean.probe(Lean::subjectProbe);
  if(changed)
    view.setPose(pos,pose);
  return changed;
  }

bool MdlVisual::updateAim(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& aim  = pose.aimIk();
  if(!aim.isValid()) {
    if(npc.isPlayer())
      Aim::playerProbe = Aim::Probe();
    if(&npc==Aim::probeNpc)
      Aim::subjectProbe = Aim::Probe();
    return false;
    }

  // drawing the bow at the target (a crossbow, a spell): where the shot leaves to, as World shoots it.
  // From where the bow is held in the stance, not from the hand: the turn moves the hand, the aim would
  // chase itself (straight down most of all)
  const float a  = npc.rotationRad();
  const auto  bs = npc.bodyStateMasked() & BS_MAX;
  const auto  ws = npc.weaponState();
  const Npc*  t  = npc.target();
  // aimed by hand (game/archery): at the crosshair, else at the target
  const bool  manual = npc.hasManualAim();
  Aim::Input in;
  in.dt      = dt;
  in.forward = {std::cos(a), 0, std::sin(a)};
  if((manual || (t!=nullptr && t!=&npc)) && !npc.isDown()) {
    // only while aiming: the target may not be valid yet (a save loading)
    auto aimAt = [&]() { return manual ? npc.manualAimPoint() : t->collosionCenter(); };
    float shoulder = 145; // human, bind pose
    if(skeleton!=nullptr) {
      const size_t sh = skeleton->findNode("BIP01 R UPPERARM");
      if(sh<skeleton->tr.size())
        shoulder = skeleton->tr[sh].at(3,1);
      }
    const Tempest::Vec3 from = npc.position() + Tempest::Vec3(0,shoulder,0) + in.forward*35.f;
    // a shot is a frame of BS_HIT, the reload goes on aiming
    if((ws==WeaponState::Bow || ws==WeaponState::CBow) && (bs==BS_AIMNEAR || bs==BS_AIMFAR || bs==(BS_HIT & BS_MAX))) {
      in.active = true;
      in.arrow  = true;
      if(manual) {
        // by hand: the arrow straight at the crosshair (the archer holds over by himself), from where
        // the arrow is (not too close: the turn moves it)
        in.dir    = npc.aimDirection(aimAt());
        in.referenceDir = aimAt()-from;
        in.sway   = npc.manualAimSway();
        in.tremor = npc.manualAimTremor();
        in.manual = true;
        in.follow = (1.f - CrouchSlow*npc.aimCrouch())*(1.f + CantQuick*(ws==WeaponState::Bow ? npc.aimCant() : 0.f));
        in.reload = pose.animProgress(ws==WeaponState::CBow ? Archery::CBowReload : Archery::BowReload,npc.world().tickCount())>=0.f;
        in.holdOrigin = npc.archer().stage()==Archer::Reloading;
        // the shot leaves along the arrow (the bolt) as it lies: in the crossbow it is in the left hand
        in.slot   = ammunition.boneId;
        } else {
        in.dir  = World::bulletVelocity(from,aimAt());
        }
      }
    else if(ws==WeaponState::Mage && bs==BS_CASTING) {
      in.active = true;
      in.dir    = aimAt() - from;
      }
    if(in.active && manual && npc.aimPeek()!=0.f) {
      // round a corner: lean out sideways as far as there is room beside the head
      const float           peek  = npc.aimPeek();
      const Tempest::Vec3   right = Tempest::Vec3::normalize(Tempest::Vec3::crossProduct(Tempest::Vec3(0,1,0),in.forward));
      // from where the head is upright (not leaned: the room does not chase itself)
      const Tempest::Vec3   head  = npc.position() + Tempest::Vec3(0,shoulder+HeadOverShoulder,0);
      const Tempest::Vec3   dir   = right*(peek>0.f ? 1.f : -1.f);
      const float           reach = Aim::SideReach + PeekClear;
      auto                  r     = npc.world().physic()->ray(head,head+dir*reach);
      const float           room  = r.hasCol ? std::clamp(((r.v-head).length()-PeekClear)/Aim::SideReach,0.f,1.f) : 1.f;
      in.side = peek*room*Aim::SideMax*float(M_PI)/180.f;
      }
    if(in.active) {
      if(npc.isPlayer() && manual) {
        in.leanDirection = npc.aimLeanDirection(aimAt());
        in.lean = npc.aimLean(aimAt(),true).angle;
        if(Aim::playerLeanOverride>=0.f)
          in.lean = Aim::playerLeanOverride;
        } else {
        // Classic aiming and NPCs keep their original angular rule and room check.
        static constexpr float reach = 58.f, drop = 28.f;
        const Tempest::Vec3 d = in.dir;
        const float down = -std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z));
        in.lean = Aim::leanForNpc(down);
        if(in.lean>0.f) {
          const float k = in.lean/(Aim::NpcLeanMax*float(M_PI)/180.f);
          const auto dir = Tempest::Vec3::normalize(Tempest::Vec3::normalize(Tempest::Vec3(d.x,0,d.z)) + in.forward*0.25f);
          const auto head = mapHeadBone();
          const auto to = head + dir*(reach*k) - Tempest::Vec3(0,drop*k,0);
          if(npc.world().physic()->ray(head,to).hasCol)
            in.lean = 0;
          }
        }
      }
    }
  // steps with the bow drawn: the legs walk under the aim (graphics/mesh/aimwalk), sneaking with the bow
  // (the crossbow), slowly
  AimWalk::Input wk;
  wk.dt   = dt;
  wk.active = manual && in.active;
  const float requestedWalk = manual && in.active ? npc.aimWalk() : 0.f;
  Tempest::Vec3 walkDirection(0,0,requestedWalk);
  if(requestedWalk==0.f && pose.aimWalk().isActive())
    walkDirection = pose.aimWalk().move(dt);
  const bool edgeBlocked = manual && in.active && npc.isEdgeMoveBlocked(walkDirection);
  wk.blocked = edgeBlocked || npc.isAimWalkStopped();
  wk.walk = edgeBlocked ? 0.f : requestedWalk;
  // Keep the gait while the stopped legs ease back into the stance, without further root motion.
  if(wk.walk!=0.f || pose.aimWalk().isActive()) {
    const bool cbow = ws==WeaponState::CBow;
    wk.legs = solver.solveFrm(cbow ? "S_CBOWSNEAKL" : "S_BOWSNEAKL");
    if(wk.legs==nullptr)
      wk.legs = solver.solveFrm(cbow ? "S_CBOWWALKL" : "S_BOWWALKL");
    }
  bool changed = pose.updateAimWalk(wk);
  if(npc.isPlayer())
    pose.aimWalk().probe(AimWalk::playerProbe);
  changed |= pose.updateAim(in);
  if(npc.isPlayer())
    aim.probe(Aim::playerProbe);
  if(&npc==Aim::probeNpc)
    aim.probe(Aim::subjectProbe);

  // a bow drawn by hand: the hand goes to the string as far as it is not drawn, the string with it (a
  // crossbow is not drawn). The bow of G2 is drawn by its morph: S_READY holds it as the animation does,
  // StringDrawn straight back as the bow is held by hand (graphics/mesh/bowdraw)
  auto&          ar = npc.archer();
  BowDraw::Input bd;
  bd.dt     = dt;
  bd.active = manual && in.active && ws==WeaponState::Bow;
  bd.ready  = ar.stage()==Archer::Nocked || ar.stage()==Archer::Drawing || ar.stage()==Archer::Held;
  bd.draw   = ar.draw();
  bd.span   = bowSpan();
  bd.arrow  = !ammunition.view.isEmpty() && ammunition.bone=="ZS_RIGHTHAND";
  bd.cant   = npc.aimCant();
  if(auto p = bow.view.protoMesh()) {
    bool high = false, straight = false;
    for(auto& m:p->morph) {
      if(m.name==BowHeld) {
        high          = true;
        bd.stringHigh = m.peakMove;
        }
      if(m.name==BowDraw::StringDrawn) {
        straight      = true;
        bd.stringRest = m.peakAt;
        bd.stringPull = m.peakMove;
        }
      }
    // along the limbs: the longest way of the bow
    const Tempest::Vec3 ext = p->bboxMesh[1]-p->bboxMesh[0];
    bd.bowAxis   = ext.x>=ext.y && ext.x>=ext.z ? Tempest::Vec3(1,0,0) : (ext.y>=ext.z ? Tempest::Vec3(0,1,0) : Tempest::Vec3(0,0,1));
    bd.hasString = high && straight;
    }
  if(ar.stage()==Archer::Reloading) {
    // how far the reload is: not begun (the shot), the animation, over (the bow comes to rest)
    const float p = pose.animProgress("T_BOWRELOAD",npc.world().tickCount());
    bd.reload = p>=0.f ? p : (ar.isNocking() ? 1.f : 0.f);
    }
  changed  |= pose.updateBowDraw(bd);
  // the string as drawn by hand: straight back as far as the bow is held so, else as G2 draws it
  const float string = pose.bowDrawIk().string();
  const float held   = pose.bowDrawIk().aligned();
  if(string>=0.f) {
    bow.view.setMMShape(BowHeld,0,0,0,string*(1.f-held));
    bow.view.setMMShape(BowDraw::StringDrawn,0,0,0,string*held);
    stringByHand = true;
    }
  else if(stringByHand) {
    // let go: the string goes (the shot: it trembles at once, the animation's release is late), else the
    // animation's morphs from here
    bow.view.startMMAnim(BowHeld,0,0);
    bow.view.startMMAnim(BowDraw::StringDrawn,0,0);
    if(pose.bowDrawIk().loosed()) {
      bow.view.startMMAnim(held>0.5f ? BowDraw::StringLoose : BowLoose,1,uint64_t(-1));
      looseAt = npc.world().tickCount();
      }
    stringByHand = false;
    }
  if(npc.isPlayer())
    pose.bowDrawIk().probe(BowDraw::playerProbe);

  if(changed)
    view.setPose(pos,pose);
  return changed;
  }

bool MdlVisual::updateFlinch(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& fl   = pose.flinchIk();
  if(!fl.isValid()) {
    if(npc.isPlayer())
      Flinch::playerProbe = Flinch::Probe();
    if(&npc==Flinch::probeNpc)
      Flinch::subjectProbe = Flinch::Probe();
    return false;
    }
  const float a = npc.rotationRad();
  Flinch::Input in;
  in.dt       = dt;
  in.forward  = {std::cos(a), 0, std::sin(a)};
  in.headOnly = pose.footIk().isQuadruped();
  const bool changed = pose.updateFlinch(in);
  if(npc.isPlayer())
    fl.probe(Flinch::playerProbe);
  if(&npc==Flinch::probeNpc)
    fl.probe(Flinch::subjectProbe);
  if(changed)
    view.setPose(pos,pose);
  return changed;
  }

void MdlVisual::flinch(const Npc& npc, const Flinch::Hit& hit) {
  const float a = npc.rotationRad();
  skInst->flinchIk().hit(hit,{std::cos(a), 0, std::sin(a)});
  }

bool MdlVisual::hasGaze() const {
  return Gaze::isEnabled() && skInst->gazeIk().isValid();
  }

bool MdlVisual::hasQuadrupedIk() const {
  return FootIk::isEnabled() && skInst->footIk().isQuadruped();
  }

bool MdlVisual::updateFootIk(Npc& npc, World& world, uint64_t dt, bool alignedToGround) {
  Pose& pose = *skInst;
  auto& ik   = pose.footIk();
  if(!ik.isValid()) {
    if(npc.isPlayer())
      ik.probe(FootIk::playerProbe,FootIk::Input());
    return false;
    }

  auto       in      = ik.mkInput(npc,world,pos,alignedToGround,dt,&pose.aimWalk());
  // crouched stepping with the bow drawn: the steps are low already (sneaking), as low as crouched standing
  in.crouch = std::max(0.f, in.crouch - pose.aimWalk().lowered());
  const bool changed = pose.updateFootIk(in);
  ik.debugLog(npc.displayName(),in,npc.isPlayer());
  if(npc.isPlayer())
    ik.probe(FootIk::playerProbe,in);
  if(changed)
    view.setPose(pos,pose);
  return changed;
  }

void MdlVisual::processLayers(World& world) {
  Pose&    pose      = *skInst;
  uint64_t tickCount = world.tickCount();
  pose.processLayers(solver,tickCount);
  }

bool MdlVisual::processEvents(World& world, uint64_t &barrier, Animation::EvCount &ev) {
  Pose&    pose      = *skInst;
  uint64_t tickCount = world.tickCount();
  return pose.processEvents(barrier,tickCount,ev);
  }

Vec3 MdlVisual::mapBone(const size_t boneId) const {
  Pose&  pose = *skInst;
  if(boneId==size_t(-1))
    return {pos.at(3,0), pos.at(3,1), pos.at(3,2)};

  auto mat = pose.bone(boneId);
  return {mat.at(3,0), mat.at(3,1), mat.at(3,2)};
  }

Vec3 MdlVisual::mapWeaponBone() const {
  if(fgtMode==WeaponState::Bow || fgtMode==WeaponState::CBow)
    return mapBone(ammunition.boneId);
  if(fgtMode==WeaponState::Mage && skeleton!=nullptr)
    return mapBone(skeleton->findNode("ZS_RIGHTHAND"));
  return {pos.at(3,0), pos.at(3,1), pos.at(3,2)};
  }

void MdlVisual::setLayerSpeed(const Animation::Sequence* seq, float k, uint64_t now) {
  skInst->setLayerSpeed(seq,k,now);
  }

void MdlVisual::hitStop(uint64_t until, float k) {
  skInst->hitStop(until,k);
  }

void MdlVisual::setAnimSpeed(std::string_view name, float k) {
  skInst->setAnimSpeed(name,k);
  }

float MdlVisual::bowSpan() const {
  // how far the string goes from rest to held drawn: the string's middle moves the most
  if(auto p = bow.view.protoMesh())
    for(auto& m:p->morph)
      if(m.name==BowHeld)
        return m.peak;
  return 0;
  }

bool MdlVisual::arrowRay(Vec3& p, Vec3& d) const {
  // the arrow (the bolt) lies along -X of its slot, the nock at the string (graphics/mesh/aim)
  size_t id = ammunition.boneId;
  if(id==size_t(-1) && skeleton!=nullptr)
    id = skeleton->findNode("ZS_RIGHTHAND");
  if(id==size_t(-1) || id>=skInst->boneCount())
    return false;
  const auto& m = skInst->bone(id);
  p = Vec3(m.at(3,0),m.at(3,1),m.at(3,2));
  d = Vec3::normalize(Vec3(-m.at(0,0),-m.at(0,1),-m.at(0,2)));
  return true;
  }

bool MdlVisual::aimAttachmentBounds(bool ammo, Matrix4x4& matrix, Vec3 (&bounds)[2]) const {
  const auto& mesh = ammo ? ammunition.view : bow.view;
  const auto* proto = mesh.protoMesh();
  if(mesh.isEmpty() || proto==nullptr)
    return false;
  matrix = mesh.node(0).position();
  bounds[0] = proto->bboxMesh[0];
  bounds[1] = proto->bboxMesh[1];
  // Frame interpolation stays within each morph's component bounds. Sum the currently
  // shown morphs, as the vertex shader does; a string pulled along the arrow must not
  // expand the bow by its full draw length sideways as well.
  for(const auto& morph:proto->morph) {
    const float weight = mesh.morphWeight(morph.name);
    bounds[0] += morph.moveMin*weight;
    bounds[1] += morph.moveMax*weight;
    }
  return true;
  }

HitZones::Zone MdlVisual::rayHitZone(const Vec3& from, const Vec3& to, float& t, size_t* bone) const {
  if(bone!=nullptr)
    *bone=size_t(-1);
  if(!hitZones.isValid()) {
    // no bones to tell: its box was hit
    t = 0;
    return HitZones::Body;
    }
  const Vec3 sx    = Vec3(pos.at(0,0),pos.at(0,1),pos.at(0,2));
  const float scale = sx.length();
  return hitZones.rayTest(skInst->transform(),skInst->boneCount(),scale>0.f ? scale : 1.f,from,to,t,bone);
  }

bool MdlVisual::hitZoneCenter(HitZones::Zone z, Vec3& at) const {
  return hitZones.center(skInst->transform(),skInst->boneCount(),z,at);
  }

float MdlVisual::animTime(std::string_view name) const {
  const Animation::Sequence* sq = solver.solveFrm(name);
  return sq!=nullptr ? sq->totalTime() : 0.f;
  }

std::string MdlVisual::animInfo(std::string_view name) const {
  const Animation::Sequence* sq = solver.solveFrm(name);
  if(sq==nullptr || sq->data==nullptr)
    return std::string(name) + ": none";
  const auto& d  = *sq->data;
  const float ms = sq->totalTime();
  char buf[512];
  std::snprintf(buf,sizeof(buf),"%s: layer %u %s frames %u fps %.1f %.0f ms nodes %zu move %d tr %.0f %.0f %.0f (%.0f cm/s) next %s",
                sq->name.c_str(),unsigned(sq->layer),sq->animCls==Animation::Loop ? "loop" : "trans",unsigned(d.numFrames),
                double(d.fpsRate),double(ms),d.nodeIndex.size(),int(d.hasMoveTr),double(d.moveTr.x),double(d.moveTr.y),
                double(d.moveTr.z),ms>0.f ? double(d.moveTr.length()/ms*1000.f) : 0.0,sq->next.c_str());
  std::string out = buf;
  // the blow's frames (the attack windows)
  for(const auto& e:d.events)
    if(e.type==asset::MdsEventType::OPTIMAL_FRAME) {
      out += "; optimal";
      for(auto f:e.frames)
        out += " " + std::to_string(f);
      }
  if(skeleton==nullptr || d.nodeIndex.empty() || d.numFrames==0)
    return out;
  // the nodes it moves (not all: an upper body only animation)
  auto& nodes = skeleton->nodes;
  if(d.nodeIndex.size()<nodes.size()) {
    out += "; moves";
    for(auto i:d.nodeIndex)
      if(i<nodes.size())
        out += " " + nodes[i].name;
    }
  // the hips at 0, 1/4, 1/2, 3/4: the thighs' line, degrees of yaw in the model (the bind pose 0); the
  // root's height
  const size_t lt = skeleton->findNode("BIP01 L THIGH");
  const size_t rt = skeleton->findNode("BIP01 R THIGH");
  if(lt>=nodes.size() || rt>=nodes.size())
    return out;
  std::vector<Tempest::Matrix4x4> local(nodes.size()), world(nodes.size());
  auto yaw = [&](const Tempest::Matrix4x4* m) {
    const float dx = m[lt].at(3,0)-m[rt].at(3,0);
    const float dz = m[lt].at(3,2)-m[rt].at(3,2);
    return std::atan2(dz,dx)*180.f/float(M_PI);
    };
  // f: a frame, else the bind pose
  auto pose = [&](uint32_t f) {
    for(size_t i=0; i<nodes.size(); ++i)
      local[i] = nodes[i].tr;
    for(size_t i=0; i<d.nodeIndex.size() && f<d.numFrames; ++i)
      if(d.nodeIndex[i]<nodes.size())
        local[d.nodeIndex[i]] = mkMatrix(d.samples[size_t(f)*d.nodeIndex.size()+i]);
    std::vector<bool> done(nodes.size(),false);
    for(size_t i=0; i<nodes.size(); ++i) {
      size_t chain[Resources::MAX_NUM_SKELETAL_NODES];
      size_t n = 0;
      for(size_t b=i; b<nodes.size() && !done[b] && n<nodes.size(); b=nodes[b].parent)
        chain[n++] = b;
      while(n>0) {
        const size_t b = chain[--n];
        const size_t p = nodes[b].parent;
        world[b] = p<nodes.size() ? world[p]*local[b] : local[b];
        done[b]  = true;
        }
      }
    };
  pose(uint32_t(-1));
  std::snprintf(buf,sizeof(buf),"; hips bind %.0f, at frames",double(yaw(world.data())));
  out += buf;
  for(uint32_t q=0; q<4; ++q) {
    const uint32_t f = std::min(d.numFrames-1,d.numFrames*q/4);
    pose(f);
    std::snprintf(buf,sizeof(buf)," %u:%.0f/%.0f",unsigned(f),double(yaw(world.data())),double(world[0].at(3,1)));
    out += buf;
    }
  return out;
  }

std::string MdlVisual::bowDump() const {
  // points of the bow and the bow hand in the frame of the arrow: x along it from the slot it sits in
  // (forward), y up across it, z to the side
  auto proto = bow.view.protoMesh();
  if(proto==nullptr || skeleton==nullptr || bow.boneId>=skInst->boneCount())
    return "bow: none";
  const size_t aSlot = skeleton->findNode("ZS_RIGHTHAND");
  if(aSlot>=skInst->boneCount())
    return "bow: no arrow slot";
  const auto& am = skInst->bone(aSlot);
  const Vec3  o  = Vec3(am.at(3,0),am.at(3,1),am.at(3,2));
  const Vec3  ax = Vec3::normalize(Vec3(-am.at(0,0),-am.at(0,1),-am.at(0,2)));
  Vec3        up = Vec3(0,1,0) - ax*Vec3::dotProduct(Vec3(0,1,0),ax);
  up             = Vec3::normalize(up);
  const Vec3  sd = Vec3::crossProduct(ax,up);
  const auto& bm = skInst->bone(bow.boneId);
  std::string out;
  char        buf[200];
  auto put = [&](const char* name, Vec3 p) {
    p = p - o;
    std::snprintf(buf,sizeof(buf),"  %-14s %6.1f %6.1f %6.1f\n",name,double(Vec3::dotProduct(p,ax)),double(Vec3::dotProduct(p,up)),
                  double(Vec3::dotProduct(p,sd)));
    out += buf;
    };
  auto inBow = [&](Vec3 p) {
    bm.project(p);
    return p;
    };
  auto dirOf = [&](const Vec3& d) {
    return Vec3(bm.at(0,0)*d.x + bm.at(1,0)*d.y + bm.at(2,0)*d.z,
                bm.at(0,1)*d.x + bm.at(1,1)*d.y + bm.at(2,1)*d.z,
                bm.at(0,2)*d.x + bm.at(1,2)*d.y + bm.at(2,2)*d.z);
    };
  const Vec3* bb = proto->bboxMesh;
  std::snprintf(buf,sizeof(buf),"bow %s: bbox %.1f %.1f %.1f .. %.1f %.1f %.1f (mesh)\n",proto->fname.c_str(),
                double(bb[0].x),double(bb[0].y),double(bb[0].z),double(bb[1].x),double(bb[1].y),double(bb[1].z));
  out += buf;
  const Vec3 mid = (bb[0]+bb[1])*0.5f;
  for(int i=0; i<3; ++i) {
    const Vec3 e  = i==0 ? Vec3(1,0,0)        : (i==1 ? Vec3(0,1,0)        : Vec3(0,0,1));
    const Vec3 lo = i==0 ? Vec3(bb[0].x,mid.y,mid.z) : (i==1 ? Vec3(mid.x,bb[0].y,mid.z) : Vec3(mid.x,mid.y,bb[0].z));
    const Vec3 hi = i==0 ? Vec3(bb[1].x,mid.y,mid.z) : (i==1 ? Vec3(mid.x,bb[1].y,mid.z) : Vec3(mid.x,mid.y,bb[1].z));
    const Vec3 d = dirOf(e);
    std::snprintf(buf,sizeof(buf),"  mesh axis %d in the arrow frame: %.2f %.2f %.2f\n",i,double(Vec3::dotProduct(d,ax)),
                  double(Vec3::dotProduct(d,up)),double(Vec3::dotProduct(d,sd)));
    out += buf;
    char n0[16], n1[16];
    std::snprintf(n0,sizeof(n0),"bbox-%d",i);
    std::snprintf(n1,sizeof(n1),"bbox+%d",i);
    put(n0,inBow(lo));
    put(n1,inBow(hi));
    }
  put("grip (slot)",inBow(Vec3()));
  put("bbox middle",inBow((bb[0]+bb[1])*0.5f));
  for(auto& m:proto->morph) {
    std::snprintf(buf,sizeof(buf),"  morph %s: peak %.1f at %.1f %.1f %.1f move %.1f %.1f %.1f (mesh)\n",m.name.c_str(),double(m.peak),
                  double(m.peakAt.x),double(m.peakAt.y),double(m.peakAt.z),double(m.peakMove.x),double(m.peakMove.y),double(m.peakMove.z));
    out += buf;
    if(m.name==BowHeld) {
      put("string rest",inBow(m.peakAt));
      put("string drawn",inBow(m.peakAt+m.peakMove));
      }
    if(m.name==BowDraw::StringDrawn) {
      put("straight rest",inBow(m.peakAt));
      put("straight drawn",inBow(m.peakAt+m.peakMove));
      }
    }
  if(ammunition.boneId<skInst->boneCount()) {
    // the arrow (the bolt) as it lies: where the shot leaves from, along what
    const auto& m = skInst->bone(ammunition.boneId);
    const Vec3  d = Vec3::normalize(Vec3(-m.at(0,0),-m.at(0,1),-m.at(0,2)));
    std::snprintf(buf,sizeof(buf),"  ammo in %s%s, along %.3f %.3f %.3f (the arrow frame)\n",std::string(ammunition.bone).c_str(),
                  ammunition.view.isEmpty() ? " (not shown)" : "",double(Vec3::dotProduct(d,ax)),double(Vec3::dotProduct(d,up)),
                  double(Vec3::dotProduct(d,sd)));
    out += buf;
    put("ammo",Vec3(m.at(3,0),m.at(3,1),m.at(3,2)));
    }
  for(auto name:{"BIP01 L HAND","BIP01 L FINGER0","BIP01 L FINGER1","BIP01 L FINGER2","BIP01 L FOREARM","BIP01 R HAND","BIP01 HEAD"}) {
    const size_t id = skeleton->findNode(name);
    if(id<skInst->boneCount()) {
      const auto& m = skInst->bone(id);
      put(name+6,Vec3(m.at(3,0),m.at(3,1),m.at(3,2)));
      }
    }
  return out;
  }

void MdlVisual::hitZonesPosed(std::vector<HitZones::Posed>& out) const {
  const Vec3 sx = Vec3(pos.at(0,0),pos.at(0,1),pos.at(0,2));
  hitZones.posed(skInst->transform(),skInst->boneCount(),sx.length(),out);
  }

std::string MdlVisual::hitZonesDump() const {
  const Vec3 sx = Vec3(pos.at(0,0),pos.at(0,1),pos.at(0,2));
  auto       body = view.protoMesh();
  return hitZones.dump(skInst->transform(),skInst->boneCount(),sx.length()) +
         hitZones.coverage(skInst->transform(),skInst->boneCount(),sx.length()) +
         " body " + (body!=nullptr ? body->fname : std::string("-"));
  }

float MdlVisual::aimTrackError() const {
  return skInst->aimIk().trackError();
  }

bool MdlVisual::isAimSteady() const {
  return skInst->aimIk().isSteady();
  }

bool MdlVisual::isAimValid() const {
  return skInst->aimIk().isValid() && Aim::isEnabled();
  }

Vec3 MdlVisual::mapHeadBone() const {
  if(skeleton->BIP01_HEAD==size_t(-1))
    return {pos.at(3,0), pos.at(3,1)+180, pos.at(3,2)};
  return mapBone(skeleton->BIP01_HEAD);
  }

void MdlVisual::stopAnim(Npc& npc, std::string_view anim) {
  skInst->stopAnim(anim);
  // Avoid issues with pfx on/off events within looped animations. Such as for Rupert and Bulko.
  if(anim.empty())
    effects.clear();
  if(!skInst->hasAnim())
    startAnimAndGet(npc,AnimationSolver::Idle,0,fgtMode,npc.walkMode());
  }

bool MdlVisual::stopItemStateAnim(Npc& npc) {
  if(!skInst->stopItemStateAnim(solver,npc.world().tickCount()))
    return false;
  if(!skInst->hasAnim())
    startAnimAndGet(npc,AnimationSolver::Idle,0,fgtMode,npc.walkMode());
  return true;
  }

void MdlVisual::stopWalkAnim(Npc &npc) {
  skInst->stopWalkAnim();
  if(!skInst->hasAnim())
    startAnimAndGet(npc,AnimationSolver::Idle,0,fgtMode,npc.walkMode());
  }

bool MdlVisual::isStanding() const {
  return skInst->isStanding();
  }

bool MdlVisual::hasAnim(std::string_view scheme) const {
  return solver.solveFrm(scheme)!=nullptr;
  }

bool MdlVisual::hasAnim(AnimationSolver::Anim a, WeaponState st, WalkBit wlk) const {
  return solver.solveAnim(a,st,wlk,*skInst)!=nullptr;
  }

const Animation::Sequence* MdlVisual::startAnimAndGet(std::string_view name, uint64_t tickCount, bool forceAnim) {
  auto sq = solver.solveFrm(name);
  if(sq!=nullptr) {
    const Pose::StartHint hint = Pose::StartHint(forceAnim  ? Pose::Force : Pose::NoHint);
    if(skInst->startAnim(solver,sq,0,BS_NONE,hint,tickCount)) {
      return sq;
      }
    }
  return nullptr;
  }

const Animation::Sequence* MdlVisual::startAnimAndGet(Npc &npc, std::string_view name, uint8_t comb, BodyState bs) {
  const Animation::Sequence* sq  = solver.solveFrm(name);
  if(skInst->startAnim(solver,sq,comb,bs,Pose::NoHint,npc.world().tickCount()))
    return sq;
  return nullptr;
  }

const Animation::Sequence* MdlVisual::startAnimAndGet(Npc& npc, AnimationSolver::Anim a,
                                                      uint8_t comb,
                                                      WeaponState st, WalkBit wlk) {
  // for those use MdlVisual::setRotation
  assert(a!=AnimationSolver::Anim::RotL && a!=AnimationSolver::Anim::RotR &&
         a!=AnimationSolver::Anim::WhirlL && a!=AnimationSolver::Anim::WhirlR);

  if(a==AnimationSolver::InteractIn ||
     a==AnimationSolver::InteractOut ||
     a==AnimationSolver::InteractToStand ||
     a==AnimationSolver::InteractFromStand) {
    auto inter = npc.interactive();
    const Animation::Sequence *sq = solver.solveAnim(inter,a,*skInst);
    if(sq!=nullptr){
      auto bs=inter->isLadder() ? BS_CLIMB : BS_MOBINTERACT;
      if(skInst->startAnim(solver,sq,comb,bs,Pose::NoHint,npc.world().tickCount()))
        return sq;
      }
    return nullptr;
    }

  if(a==AnimationSolver::Anim::NoAnim) {
    skInst->stopAllAnim();
    return nullptr;
    }

  const Animation::Sequence *sq = solver.solveAnim(a,st,wlk,*skInst,&npc);
  if(sq==nullptr)
    return nullptr;

  bool forceAnim=false;
  if(a==AnimationSolver::Anim::DeadA        || a==AnimationSolver::Anim::DeadB ||
     a==AnimationSolver::Anim::UnconsciousA || a==AnimationSolver::Anim::UnconsciousB ||
     a==AnimationSolver::Anim::FallDeepA    || a==AnimationSolver::Anim::FallDeepB) {
    skInst->stopAllAnim();
    forceAnim = true;
    }
  if(a==AnimationSolver::Anim::JumpHang) {
    forceAnim = true;
    }

  BodyState bs = BS_NONE;
  switch(a) {
    case AnimationSolver::Anim::NoAnim:
      bs = BS_NONE;
      break;
    case AnimationSolver::Anim::Fallen:
    case AnimationSolver::Anim::FallenA:
    case AnimationSolver::Anim::FallenB:
      bs = BS_LIE;
      break;
    case AnimationSolver::Anim::Idle:
    case AnimationSolver::Anim::MagNoMana:
      bs = BS_STAND;
      break;
    case AnimationSolver::Anim::Move:
    case AnimationSolver::Anim::MoveL:
    case AnimationSolver::Anim::MoveR:
      if(bool(wlk & WalkBit::WM_Walk))
        bs = BS_WALK; else
        bs = BS_RUN;
      break;
    case AnimationSolver::Anim::MoveBack:
      if(st==WeaponState::Fist || st==WeaponState::W1H || st==WeaponState::W2H)
        bs = BS_PARADE; else
        bs = BS_RUN;
    case AnimationSolver::Anim::RotL:
    case AnimationSolver::Anim::RotR:
    case AnimationSolver::Anim::WhirlL:
    case AnimationSolver::Anim::WhirlR:
      break;
    case AnimationSolver::Anim::Fall:
    case AnimationSolver::Anim::FallDeep:
    case AnimationSolver::Anim::FallDeepA:
    case AnimationSolver::Anim::FallDeepB:
      bs = BS_FALL;
      break;
    case AnimationSolver::Anim::Jump:
    case AnimationSolver::Anim::JumpUp:
      bs = BS_JUMP;
      break;
    case AnimationSolver::Anim::JumpUpLow:
    case AnimationSolver::Anim::JumpUpMid:
    case AnimationSolver::Anim::JumpHang:
      bs = BS_CLIMB;
      break;
    case AnimationSolver::Anim::SlideA:
    case AnimationSolver::Anim::SlideB:
      bs = BS_NONE;
      break;
    case AnimationSolver::Anim::DeadA:
    case AnimationSolver::Anim::DeadB:
      bs = BS_DEAD;
      break;
    case AnimationSolver::Anim::UnconsciousA:
    case AnimationSolver::Anim::UnconsciousB:
      bs = BS_UNCONSCIOUS;
      break;
    case AnimationSolver::Anim::InteractIn:
    case AnimationSolver::Anim::InteractOut:
    case AnimationSolver::Anim::InteractToStand:
    case AnimationSolver::Anim::InteractFromStand:
      bs = BS_MOBINTERACT;
      break;
    case AnimationSolver::Anim::Attack:
    case AnimationSolver::Anim::AttackL:
    case AnimationSolver::Anim::AttackR:
    case AnimationSolver::Anim::AttackFinish:
      bs = BS_HIT;
      break;
    case AnimationSolver::Anim::AttackBlock:
      bs = BS_PARADE;
      break;
    case AnimationSolver::Anim::StumbleA:
    case AnimationSolver::Anim::StumbleB:
      bs = BS_STUMBLE;
      break;
    case AnimationSolver::Anim::AimBow:
    case AnimationSolver::Anim::ReloadBow:
      bs = BS_AIMNEAR; //TODO: BS_AIMFAR
      break;
    case AnimationSolver::Anim::ItmGet:
      bs = BS_TAKEITEM;
      break;
    case AnimationSolver::Anim::ItmDrop:
      bs = BS_DROPITEM;
      break;
    case AnimationSolver::Anim::PointAt:
      bs = BS_STAND;
      break;
    }

  if(bool(wlk & WalkBit::WM_Dive))
    bs = BS_DIVE;
  else if(bool(wlk & WalkBit::WM_Swim))
    bs = BS_SWIM;
  else if(bool(wlk & WalkBit::WM_Sneak) && (st!=WeaponState::Bow && st!=WeaponState::CBow))
    bs = BS_SNEAK;

  Pose::StartHint hint = (forceAnim ? Pose::Force : Pose::NoHint);

  if(skInst->startAnim(solver,sq,comb,bs,hint,npc.world().tickCount()))
    return sq;
  return nullptr;
  }

bool MdlVisual::startAnim(Npc &npc, WeaponState st) {
  const bool run = (skInst->bodyState()&BS_MAX)==BS_RUN;

  if(st==fgtMode)
    return true;
  const Animation::Sequence *sq = solver.solveAnim(st,fgtMode,run);
  if(sq==nullptr)
    return false;
  if(skInst->startAnim(solver,sq,0,run ? BS_RUN : BS_STAND,Pose::NoHint,npc.world().tickCount()))
    return true;
  return false;
  }

void MdlVisual::setAnimRotate(Npc &npc, int dir) {
  skInst->setAnimRotate(solver,npc,fgtMode,AnimationSolver::TurnType::Std,dir);
  }

void MdlVisual::setAnimWhirl(Npc &npc, int dir) {
  skInst->setAnimRotate(solver,npc,fgtMode,AnimationSolver::TurnType::Whirl,dir);
  }

void MdlVisual::interrupt() {
  skInst->interrupt();
  item.clear();
  setStateItem(MeshObjects::Mesh(),"");
  }

Tempest::Vec3 MdlVisual::displayPosition() const {
  if(skeleton!=nullptr)
    return {0,skeleton->colisionHeight()*1.6f,0};
  return {0.f,0.f,0.f};
  }

float MdlVisual::viewDirection() const {
  auto p = pos;
  if(nullptr!=skeleton) {
    size_t nodeId = skeleton->findNode("BIP01");
    if(nodeId!=size_t(-1))
      p = pose().bone(nodeId);
    }
  float rx = p.at(2,0);
  float rz = p.at(2,2);
  return float(std::atan2(rz,rx)) * 180.f / float(M_PI) - 90.f;
  }

const Animation::Sequence* MdlVisual::continueCombo(Npc& npc, AnimationSolver::Anim a, BodyState bs,
                                                    WeaponState st, WalkBit wlk)  {
  if(st==WeaponState::Fist || st==WeaponState::W1H || st==WeaponState::W2H) {
    const Animation::Sequence *sq = solver.solveAnim(a,st,wlk,*skInst,&npc);
    if(auto ret = skInst->continueCombo(solver,sq,bs,npc.world().tickCount()))
      return ret;
    }
  return startAnimAndGet(npc,a,0,st,wlk);
  }

uint16_t MdlVisual::comboLength() const {
  return skInst->comboLength();
  }

Bounds MdlVisual::bounds() const {
  auto b = view.bounds();
  if(!head.view.isEmpty())
    b.assign(b,head.view.bounds());
  return b;
  }

void MdlVisual::bind(MeshAttach& slot, MeshObjects::Mesh&& itm, std::string_view bone) {
  slot.boneId = skeleton==nullptr ? size_t(-1) : skeleton->findNode(bone);
  slot.view   = std::move(itm);
  slot.bone   = bone;
  // sync?
  }

void MdlVisual::bind(PfxAttach& slot, Effect&& itm, std::string_view bone) {
  slot.boneId = skeleton==nullptr ? size_t(-1) : skeleton->findNode(bone);
  slot.view   = std::move(itm);
  slot.bone   = bone;
  // sync?
  }

template<class View>
void MdlVisual::bind(Attach<View>& slot, std::string_view bone) {
  slot.boneId = skeleton==nullptr ? size_t(-1) : skeleton->findNode(bone);
  slot.bone   = bone;
  // sync?
  }

void MdlVisual::rebindAttaches(const Skeleton& to) {
  auto  mesh = {&head,&sword,&bow,&ammunition,&stateItm};
  for(auto i:mesh)
    rebindAttaches(*i,to);
  for(auto& i:item)
    rebindAttaches(i,to);
  for(auto& i:attach)
    rebindAttaches(i,to);
  for(auto& i:effects)
    i.view.bindAttaches(*skInst,to);
  pfx.view.bindAttaches(*skInst,to);
  hnpcVisual.view.bindAttaches(*skInst,to);
  }

template<class View>
void MdlVisual::rebindAttaches(Attach<View>& mesh, const Skeleton& to) {
  if(mesh.bone.empty())
    mesh.boneId = size_t(-1); else
    mesh.boneId = to.findNode(mesh.bone);
  }

void MdlVisual::syncAttaches() {
  MdlVisual::MeshAttach* mesh[] = {&head, &sword,&shield,&bow,&ammunition,&stateItm};
  for(auto i:mesh)
    syncAttaches(*i);
  for(auto& i:item)
    syncAttaches(i);
  for(auto& i:attach)
    syncAttaches(i);
  for(auto& pin:stuckArrows)
    pin.view.setObjMatrix(renderedBone(pin.bone)*pin.local);
  for(auto& i:effects) {
    i.view.setObjMatrix(pos);
    // i.view.setTarget(targetPos);
    }
  pfx.view.setObjMatrix(pos);
  hnpcVisual.view.setObjMatrix(pos);
  if(torch.view!=nullptr) {
    auto& pose = *skInst;
    auto  p    = pos;
    if(torch.boneId<pose.boneCount())
      p = renderedBone(torch.boneId);
    torch.view->setObjMatrix(p);
    }
  }

const Skeleton* MdlVisual::visualSkeleton() const {
  return skeleton;
  }

std::string_view MdlVisual::visualSkeletonScheme() const {
  if(skeleton==nullptr)
    return "";
  auto ret = skeleton->name();
  auto end = ret.find_first_of("._");
  return ret.substr(0, end);
  }

template<class View>
void MdlVisual::syncAttaches(Attach<View>& att) {
  if(att.view.isEmpty())
    return;
  auto& pose = *skInst;
  auto  p    = pos;
  if(att.boneId<pose.boneCount())
    p = renderedBone(att.boneId);
  att.view.setObjMatrix(p);
  }

const Animation::Sequence* MdlVisual::startAnimItem(Npc &npc, std::string_view scheme, int state) {
  return skInst->setAnimItem(solver,npc,scheme,state);
  }

const Animation::Sequence* MdlVisual::startAnimSpell(Npc &npc, std::string_view scheme, bool invest) {
  const bool run = (skInst->bodyState()&BS_MAX)==BS_RUN; // not really the case, as in Gothic player can't cast spell, while running

  const Animation::Sequence *sq = solver.solveAnim(scheme,run,invest);
  if(skInst->startAnim(solver,sq,0,BS_CASTING,Pose::NoHint,npc.world().tickCount())) {
    return sq;
    }
  return nullptr;
  }

namespace {
// T_DIALOGGESTURE_01..
std::array<char,32> gestureName(uint16_t id) {
  std::array<char,32> name = {};
  std::snprintf(name.data(),name.size(),"T_DIALOGGESTURE_%02d",id);
  return name;
  }
}

void MdlVisual::setupGestures() {
  // how long each gesture plays; an alias of one before it (Gothic II: 11 is 01) is left out
  const uint16_t count = Gothic::inst().version().dialogGestureCount();
  std::vector<uint32_t>                   time(count);
  std::vector<const Animation::AnimData*> seen;
  for(uint16_t i=0; i<count; ++i) {
    auto sq = solver.solveFrm(gestureName(uint16_t(i+1)).data());
    if(sq==nullptr || sq->data==nullptr || std::find(seen.begin(),seen.end(),sq->data.get())!=seen.end())
      continue;
    seen.push_back(sq->data.get());
    time[i] = uint32_t(sq->totalTime());
    }
  gestures.setGestures(std::move(time),Gothic::inst().version().game==2);
  }

bool MdlVisual::startAnimDialog(Npc &npc) {
  if(TalkGestures::isEnabled()) {
    // the gestures go with the line from now on (updateGestures)
    setupGestures();
    gestures.lineStart(npc.world().tickCount());
    return true;
    }

  if((npc.bodyState()&BS_FLAG_FREEHANDS)==0 || fgtMode!=WeaponState::NoWeapon)
    return true;
  if(npc.bodyStateMasked()!=BS_STAND)
    return true;

  const uint16_t count = Gothic::inst().version().dialogGestureCount();
  const int      id    = std::rand()%count + 1;

  const Animation::Sequence *sq   = solver.solveFrm(gestureName(uint16_t(id)).data());
  const bool                 over = gestureSeq!=nullptr && skInst->isInAnim(gestureSeq);
  if(skInst->startAnim(solver,sq,0,BS_STAND,Pose::NoHint,npc.world().tickCount())) {
    gestureSeq = sq;
    gestures.started(uint8_t(id),over);
    return true;
    }
  return false;
  }

void MdlVisual::updateGestures(Npc& npc, uint64_t dt) {
  const bool     probing = &npc==TalkGestures::probeNpc;
  const uint64_t now     = npc.world().tickCount();
  const bool     voiced  = mouth.isSpeaking(now);
  const uint64_t left    = std::max(npc.speechLeft(),mouth.voiceLeft(now));
  if(skInst==nullptr || (gestureSeq==nullptr && gestures.isIdle(now) && !probing && !voiced && left==0))
    return;
  const bool playing = gestureSeq!=nullptr && skInst->isInAnim(gestureSeq);
  if(!playing)
    gestureSeq = nullptr;
  const uint32_t syl = mouth.syllableCount();
  const bool     beat = syl!=gestureSyl;
  gestureSyl = syl;

  // the head: Gaze turns it (updateGaze)
  gestures.updateHead(now,dt,voiced || left>0,beat,mouth.syllablePeak());

  TalkGestures::Input in;
  in.now      = now;
  // not over an animation of the ai queue (a nod, a shrug the AI server or a script plays)
  in.allowed  = (npc.bodyState()&BS_FLAG_FREEHANDS)!=0 && fgtMode==WeaponState::NoWeapon &&
                npc.bodyStateMasked()==BS_STAND && !npc.isPlayingQueuedAnim();
  in.voiced   = voiced;
  in.left     = left;
  in.speaking = voiced || left>0;
  in.more     = npc.hasOutputQueued();
  in.playing  = playing;
  in.syllable = beat;

  const auto d = gestures.update(in);
  if(d.act==TalkGestures::Act::Start) {
    auto sq = solver.solveFrm(gestureName(d.gesture).data());
    if(sq!=nullptr && skInst->startAnim(solver,sq,0,BS_STAND,Pose::NoHint,now))
      gestureSeq = sq;
    }
  else if(d.act==TalkGestures::Act::Stop && gestureSeq!=nullptr) {
    skInst->stopAnim(gestureSeq->name);
    gestureSeq = nullptr;
    }
  if(probing)
    gestures.probe(TalkGestures::subjectProbe);
  }

void MdlVisual::updateBlink(Npc& npc, uint64_t dt) {
  const bool probing = &npc==EyeBlink::probeNpc;
  // a far face is not seen blinking (and its morph is not worth the upload)
  if(skInst==nullptr || head.view.isEmpty() || (npc.processPolicy()>NpcProcessPolicy::AiNormal && !probing))
    return;
  const uint64_t now = npc.world().tickCount();
  const uint32_t sh  = skInst->gazeIk().shiftCount();

  EyeBlink::Input in;
  in.now      = now;
  in.dt       = dt;
  in.allowed  = !npc.isDown() && (npc.bodyStateMasked() & BS_MAX)!=BS_LIE && head.view.morphWeight("S_EYESCLOSED")<0.1f;
  in.speaking = mouth.isSpeaking(now) || npc.speechLeft()>0;
  in.voice    = mouth.voiceLevel();
  in.shift    = sh!=blinkShift;
  // a frightened face blinks more, a hostile or angry stare less
  in.rate     = 1.f + 0.6f*head.view.morphWeight("S_FRIGHTENED")
                    - 0.3f*std::max(head.view.morphWeight("S_HOSTILE"),head.view.morphWeight("S_ANGRY"));
  blinkShift  = sh;

  uint64_t hold = 0;
  if(blink.update(in,hold))
    head.view.startMMAnim("R_EYESBLINK",1,now+hold,true);  // not in place of a face
  if(probing)
    blink.probe(EyeBlink::subjectProbe);
  }

void MdlVisual::startMMAnim(Npc& npc, std::string_view anim, std::string_view bone) {
  MdlVisual::MeshAttach* mesh[] = {&head,&sword,&shield,&bow,&ammunition,&stateItm};
  for(auto i:mesh) {
    if(i->bone!=bone && !bone.empty())
      continue;
    // the string drawn by hand: the animation does not draw it; held by hand, the bow lets it go straight
    if(i==&bow && skInst->bowDrawIk().string()>=0.f)
      continue;
    if(i==&bow && anim==BowLoose && looseAt>0 && npc.world().tickCount()<looseAt+LooseLate)
      continue;
    if(i==&bow && anim==BowLoose && skInst->bowDrawIk().aligned()>0.5f) {
      i->view.startMMAnim(BowDraw::StringLoose,1,uint64_t(-1));
      continue;
      }
    i->view.startMMAnim(anim,1,uint64_t(-1));
    }
  }

void MdlVisual::startFaceAnim(Npc& npc, std::string_view anim, float intensity, uint64_t duration) {
  // a voiced line moves the mouth itself: no talk loop over it
  if(anim=="VISEME" && duration!=0 && mouth.isSpeaking(npc.world().tickCount()))
    return;
  if(duration!=uint64_t(-1) && duration!=0)
    duration += npc.world().tickCount();
  head.view.startMMAnim(anim,intensity,duration);
  }

void MdlVisual::startSpeech(Npc& npc, std::shared_ptr<LipSync::Track> voice) {
  if(voice==nullptr)
    return;
  // the mouth shapes of the head: a head without them gets the talk loop
  std::vector<float> shapes;
  if(auto proto = head.view.protoMesh())
    for(auto& m:proto->morph)
      if(m.name=="VISEME") {
        shapes = m.strength;
        break;
        }
  mouth.setShapes(std::move(shapes));
  mouth.start(std::move(voice),npc.world().tickCount());
  }

void MdlVisual::updateMouth(Npc& npc, uint64_t dt) {
  LipSync::Shape s;
  if(mouth.update(npc.world().tickCount(),dt,s))
    head.view.setMMShape("VISEME",s.frame0,s.frame1,s.alpha,s.intensity);
  if(&npc==LipSync::probeNpc)
    mouth.probe(LipSync::subjectProbe);
  }

void MdlVisual::stopDlgAnim(Npc& npc) {
  mouth.stop();
  gestures.stop();
  gestureSeq = nullptr;
  const uint16_t count = Gothic::inst().version().dialogGestureCount();
  for(uint16_t i=0; i<count; i++)
    skInst->stopAnim(gestureName(uint16_t(i+1)).data());

  if(npc.processPolicy()<=NpcProcessPolicy::AiNormal) {
    // avoid PCI traffic on distant npc's
    startFaceAnim(npc,"VISEME",1,0);
    }
  }
