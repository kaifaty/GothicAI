#include "mdlvisual.h"
#include "mdlvisual_internal.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/renderrig.h"
#include "game/serialize.h"
#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "graphics/worldview.h"
#include "objvisual.h"
#include "gothic.h"

#include <Tempest/Log>

#include <chrono>
#include <ctime>
#include "graphics/mesh/ikmath.h"
#include "frameprofile.h"
#include "worldchunksruntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <set>

using namespace Tempest;
using namespace MdlVisualInternal;

namespace MdlVisualInternal {

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

Vec3 asidePoint(const Vec3& head, const Vec3& target, float yaw, float pitch) {
  const Vec3  d   = target - head;
  const float len = std::sqrt(d.x*d.x + d.z*d.z);
  if(len<=1.f)
    return target;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const Vec3  r = {d.x*c - d.z*s, 0, d.x*s + d.z*c};
  return head + Vec3(r.x, d.y + len*std::tan(pitch), r.z);
  }

}

namespace {

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

  skeleton = v;
  const bool rigChanged = bindBodyRig();
  hitZones.setSkeleton(v);
  setObjMatrix(pos); // invalidate obj matrix
  if(rigChanged)
    syncAttaches();
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
  pushBodyPose();
  return true;
  }

bool MdlVisual::clearPhysicalPose() {
  if(renderedPose.empty())
    return false;
  renderedPose.clear();
  renderedMask.clear();
  pushBodyPose();
  return true;
  }

const Matrix4x4& MdlVisual::renderedBone(size_t bone) const {
  if(bone<renderedPose.size())
    return renderedPose[bone];
  return skInst->bone(bone);
  }

const Matrix4x4* MdlVisual::skinPalette(size_t& count) const {
  if(bodyRig!=nullptr) {
    count = rigPalette.size();
    return rigPalette.data();
    }
  count = skInst->boneCount();
  return renderedPose.empty() ? skInst->transform() : renderedPose.data();
  }

const Skeleton* MdlVisual::skinSkeleton() const {
  return bodyRig!=nullptr ? &bodyRig->skeleton() : skeleton;
  }

bool MdlVisual::bindBodyRig() {
  const RenderRig* const was   = bodyRig;
  const ProtoMesh*       proto = view.protoMesh();
  const RenderRig*       rig   = proto!=nullptr && skeleton!=nullptr ? Resources::renderRigOfMesh(proto->fname) : nullptr;
  // the palette the mesh reads: its bones are the rig's
  size_t bones = 0;
  if(proto!=nullptr)
    for(const auto& skin:proto->skined)
      bones = std::max(bones,skin.bonesCount);
  std::string why;
  if(rig!=nullptr && !rig->fits(*skeleton))
    why = "is put on \""+std::string(skeleton->name())+"\", not on the rig's source: drawn on it";
  else if(rig!=nullptr && bones>rig->bones())
    why = "has "+std::to_string(bones)+" bones, the rig "+std::to_string(rig->bones())+": drawn on the visual's skeleton";
  if(!why.empty()) {
    // once for a body, a skeleton and a rig: each NPC so dressed would repeat it
    static std::mutex            sync;
    static std::set<std::string> told;
    const std::string key = std::string(rig->name())+"|"+proto->fname+"|"+std::string(skeleton->name());
    std::lock_guard<std::mutex> guard(sync);
    if(told.insert(key).second)
      Log::e("render rig ",rig->name(),": body \"",proto->fname,"\" ",why);
    rig = nullptr;
    }
  if(bodyRig!=rig)
    rigGait = rigGaitNow = RenderRig::Gait();
  bodyRig = rig;
  rigPalette.resize(rig!=nullptr ? rig->bones() : 0);
  view.setSkeleton(rig!=nullptr ? &rig->skeleton() : skeleton);
  if(bodyRig==was)
    return false;
  // the effects on the skin (burning, spells): the new body's emitter mesh and its bones
  for(auto& e:effects)
    e.view.setMesh(&view,bodySkin());
  hnpcVisual.view.setMesh(&view,bodySkin());
  return true;
  }

void MdlVisual::pushBodyPose() {
  const Matrix4x4* source = renderedPose.empty() ? skInst->transform() : renderedPose.data();
  if(bodyRig==nullptr) {
    view.setPose(pos,source);
    return;
    }
  // the legs' memory: every push of a frame is one step from what the last frame left (rigGait), so pushing twice
  // in a frame - the place first, the pose after - does not step twice
  RenderRig::Gait gait = rigGait;
  const bool solved = bodyRig->solve(source,skInst->boneCount(),rigPalette.data(),RenderRig::Hands{rigHold,rigWorld},
                                     &gait,rigStep);
  if(solved)
    rigGaitNow = gait;
  if(!solved) {
    // the Gothic pose is not there yet: the rig's bind pose where the body stands
    const auto& bind = bodyRig->skeleton().tr;
    for(size_t i=0; i<rigPalette.size() && i<bind.size(); ++i)
      rigPalette[i] = pos*bind[i];
    }
  view.setPose(pos,rigPalette.data());
  }

Matrix4x4 MdlVisual::attachMatrix(size_t node) const {
  Matrix4x4 m;
  if(bodyRig!=nullptr && bodyRig->socket(node,rigPalette.data(),m))
    return m;
  return renderedBone(node);
  }

const Matrix4x4* MdlVisual::skinOf(const Matrix4x4* bones, size_t n, std::vector<Matrix4x4>& rig, size_t& count) const {
  if(bodyRig!=nullptr) {
    rig.resize(bodyRig->bones());
    // without the legs' memory: these poses are not the drawn one (the bind pose, a pose asked for a fit)
    if(bodyRig->solve(bones,n,rig.data(),RenderRig::Hands{rigHold,rigWorld})) {
      count = rig.size();
      return rig.data();
      }
    count = 0;  // too few Gothic bones: no skin
    return rig.data();
    }
  count = n;
  return bones;
  }

void MdlVisual::setYTranslationEnable(bool e) {
  if(e)
    skInst->setFlags(Pose::NoFlags); else
    skInst->setFlags(Pose::NoTranslation);
  }

void MdlVisual::setVisualBody(World& owner, MeshObjects::Mesh&& body) {
  implSetBody(nullptr,owner,std::move(body),0);
  }

void MdlVisual::setVisualBody(WorldView& owner, MeshObjects::Mesh&& body) {
  // Mobsi keep their solver and pose while only these draw instances leave residency.
  attach.clear();
  if(auto proto=body.protoMesh())
    for(const auto& slot:proto->attach)
      if(!slot.hasNode)
        setSlotAttachment(owner.addAtachView(slot,0),slot.name);
  view=std::move(body);
  bodyDetached=false;
  addTextureOwner(view);
  bindBodyRig();
  pushBodyPose();
  for(auto& slot:item)
    if(slot.deferredProto!=nullptr) {
      slot.view=owner.addView(slot.deferredProto,slot.deferredMaterial);
      addTextureOwner(slot.view);
      }
  syncAttaches();
  }

void MdlVisual::clearVisualBody() {
  view={};
  bindBodyRig();  // no body, no rig: the effects on its skin let go of the palette
  attach.clear();
  for(auto& slot:item) {
    if(auto proto=slot.view.protoMesh())
      slot.deferredProto=proto;
    slot.view={};
    }
  quiv.clear();
  bodyDetached=true;
  }

size_t MdlVisual::bodyNodesCount() const {
  size_t count=view.nodesCount();
  for(const auto& slot:attach)
    count+=slot.view.nodesCount();
  for(const auto& slot:item)
    count+=slot.view.nodesCount();
  return count;
  }

// Mdl_SetVisualBody
void MdlVisual::setVisualBody(Npc& npc, MeshObjects::Mesh &&h, MeshObjects::Mesh &&body, int32_t version) {
  const auto* instance=npc.world().script().findSymbol(npc.instanceSymbol());
  if(instance!=nullptr)
    if(const auto* own=Resources::loadCharacterHead(instance->name()))
      h=npc.world().addView(own);
  bind(head,std::move(h),"BIP01 HEAD");
  // a new view of the head: the face goes to it again (the anims go on)
  const auto* proto=head.view.protoMesh();
  face.setup(headMesh(),proto!=nullptr ? proto->face.get() : nullptr);
  // the jumps of the eyes repeat for the same npc (the test bench)
  eyes.setup(face.eyes(),uint32_t(npc.instanceSymbol()));
  implSetBody(&npc,npc.world(),std::move(body),version);
  head.view.setAsGhost(hnpcFlagGhost);
  head.view.setFatness(npc.fatness());
  }

const GltfHeadMesh* MdlVisual::headMesh() const {
  const auto* proto=head.view.protoMesh();
  return proto!=nullptr ? proto->head.get() : nullptr;
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
  if(npc)
    setTextureOwner(npc->position(),!npc->isPlayer());
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
  addTextureOwner(view);
  view.setAsGhost(hnpcFlagGhost);
  view.setFatness(npc==nullptr ? 0 : npc->fatness());
  const bool rigChanged = bindBodyRig();
  pushBodyPose();
  hnpcVisual.view.setMesh(&view,bodySkin());
  fitHitZones();
  if(rigChanged)
    syncAttaches();  // the head and the hands: from the rig's sockets or back on Gothic's nodes
  }

void MdlVisual::fitHitZones() {
  // the zones as thick as the body is: its mesh by bone, its parts on the bones of their names, the head's
  // mesh on its bone
  std::vector<HitZones::Shape> shape;
  // a body on its render rig: the points of a rig bone go to its Gothic node (RenderRig::sourceOf), from the rig posed
  // as Gothic's bind pose into that node's bind frame; the zones stay Gothic's
  std::vector<std::vector<Vec3>> byNode;
  std::vector<Matrix4x4>         rigBind;
  size_t                         rigCount = 0;
  if(auto p = view.protoMesh(); p!=nullptr && bodyRig!=nullptr && skeleton!=nullptr) {
    skinOf(skeleton->tr.data(),skeleton->tr.size(),rigBind,rigCount);
    byNode.resize(skeleton->tr.size());
    for(size_t r=0; r<p->nodePoints.size() && r<rigCount; ++r) {
      const size_t node = bodyRig->sourceOf(r);
      if(node>=byNode.size())
        continue;
      auto toNode = skeleton->tr[node];
      toNode.inverse();
      toNode.mul(rigBind[r]);
      for(auto pt:p->nodePoints[r]) {
        toNode.project(pt);
        byNode[node].push_back(pt);
        }
      }
    }
  if(auto p = view.protoMesh()) {
    shape.push_back({bodyRig!=nullptr && skeleton!=nullptr ? &byNode : &p->nodePoints,size_t(-1),nullptr});
    for(size_t i=0; skeleton!=nullptr && i<p->attach.size() && i<p->attachPoints.size(); ++i)
      if(!p->attach[i].hasNode)
        shape.push_back({nullptr,skeleton->findNode(p->attach[i].name),&p->attachPoints[i]});
    }
  if(auto p = head.view.protoMesh(); p!=nullptr && skeleton!=nullptr)
    shape.push_back({nullptr,skeleton->findNode("BIP01 HEAD"),&p->points});
  hitZones.fit(shape);
  }

void MdlVisual::setSlotAttachment(MeshObjects::Mesh&& itm, std::string_view bone) {
  addTextureOwner(itm);
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

void MdlVisual::setSlotItem(MeshObjects::Mesh &&itm, std::string_view bone, int32_t material) {
  addTextureOwner(itm);
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
      i.deferredProto=itm.protoMesh();
      i.deferredMaterial=material;
      if(bodyDetached)
        itm={};
      i.view=std::move(itm);
      syncAttaches();
      return;
      }
    }

  MeshAttach slt;
  slt.bone = skeleton->nodes[id].name;
  slt.deferredProto=itm.protoMesh();
  slt.deferredMaterial=material;
  if(bodyDetached)
    itm={};
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

  return setToFightMode(f,true);
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

  vfx.setMesh(&view,bodySkin());
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

void MdlVisual::setNpcEffect(World& owner, Npc& npc, std::string_view s, daedalus::NpcFlag flags) {
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
    hnpcVisual.view.setMesh(&view,bodySkin());
    }

  const bool nextGhost = (flags & daedalus::NpcFlag::GHOST);
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

bool MdlVisual::setToFightMode(const WeaponState f, bool fromAnimation) {
  if(f==fgtMode)
    return false;
  fgtMode = f;
  skInst->recordLogicalWeapon(f,fromAnimation);
  return true;
  }

void MdlVisual::setObjMatrix(const Tempest::Matrix4x4 &m, bool syncAttach) {
  FrameProfile::Scope timing(FrameProfile::Zone::PaletteCpu,FrameProfile::actorContext());
  pos = m;
  skInst->setObjectMatrix(m,syncAttach);
  pushBodyPose();
  if(syncAttach)
    syncAttaches();
  }

float MdlVisual::setTextureOwner(const Vec3& position, bool spatial) {
  if(textureOwnerKnown && textureOwnerSpatial==spatial) {
    if(!spatial)
      return std::numeric_limits<float>::infinity();
    textureOwnerLast=position;
    const auto d=position-textureOwnerPosition;
    const float moved=d.x*d.x+d.y*d.y+d.z*d.z;
    if(moved<=textureOwnerReach*textureOwnerReach)
      return textureOwnerReach-std::sqrt(moved);
    }
  textureOwnerKnown=true;
  textureOwnerSpatial=spatial;
  textureOwnerPosition=position;
  textureOwnerLast=position;
  float reach=view.setTextureOwner(position,spatial);
  MeshAttach* slots[]={&head,&sword,&shield,&bow,&ammunition,&stateItm};
  for(auto* slot:slots)
    reach=std::min(reach,slot->view.setTextureOwner(position,spatial));
  for(auto& slot:item)
    reach=std::min(reach,slot.view.setTextureOwner(position,spatial));
  for(auto& slot:attach)
    reach=std::min(reach,slot.view.setTextureOwner(position,spatial));
  for(auto& pin:stuckArrows)
    reach=std::min(reach,pin.view.setTextureOwner(position,spatial));
  reach=std::min(reach,quiv.setTextureOwner(position,spatial));
  if(torch.view)
    reach=std::min(reach,torch.view->setTextureOwner(position,spatial));
  textureOwnerReach=reach;
  return reach;
  }

bool MdlVisual::minSkinnedVertex(float fatness,Vec3& point) const {
  const auto* mesh=view.protoMesh();
  if(mesh==nullptr)
    return false;
  size_t count = 0;
  const auto* bones = skinPalette(count);
  return mesh->minSkinnedVertex(bones,count,pos,fatness,point);
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

  auto hitem = std::make_shared<daedalus::IItem>();
  owner.script().initializeInstanceItem(hitem, torchId);
  const auto* residency=owner.chunkResidency();
  Resources::TextureLoadScope textureLoadScope(textureOwnerKnown && textureOwnerSpatial && residency &&
      residency->textureDistance(ChunkId::fromPosition(textureOwnerPosition.x,textureOwnerPosition.z))>2);
  torch.view.reset(new ObjVisual());
  torch.view->setVisual(*hitem,owner,false);
  addTextureOwner(*torch.view);
  torch.boneId = (skeleton==nullptr ? size_t(-1) : skeleton->findNode("ZS_LEFTHAND"));

  auto& pose = *skInst;
  auto  p    = pos;
  if(torch.boneId<pose.boneCount())
    p = attachMatrix(torch.boneId);
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

  // the render rig's arms: one thing in both hands (a 2H weapon, a bow, a crossbow, a spell) - the left hand holds on
  // to the right as Gothic's does; at a mob (a ladder, a workbench) - the hands go onto Gothic's
  // the render rig's legs: this frame steps from what the last one left
  if(bodyRig!=nullptr) {
    rigGait = rigGaitNow;
    rigStep = float(dt)/1000.f;
    }
  bool handsMoved = false;
  if(bodyRig!=nullptr && npc!=nullptr) {
    const auto  ws    = npc->weaponState();
    const float hold  = (ws==WeaponState::W2H || ws==WeaponState::Bow || ws==WeaponState::CBow ||
                         ws==WeaponState::Mage) ? 1.f : 0.f;
    const float world = npc->interactive()!=nullptr ? 1.f : 0.f;
    const float ease  = 1.f - std::exp(-float(dt)/150.f);
    const float h = rigHold  + (hold -rigHold )*ease;
    const float w = rigWorld + (world-rigWorld)*ease;
    auto settle = [](float v, float goal) { return std::abs(v-goal)<1e-3f ? goal : v; };
    handsMoved = settle(h,hold)!=rigHold || settle(w,world)!=rigWorld;
    rigHold    = settle(h,hold);
    rigWorld   = settle(w,world);
    }

  if(changed || handsMoved) {
    FrameProfile::Scope timing(FrameProfile::Zone::PaletteCpu,FrameProfile::watchedActor(npc));
    pushBodyPose();
    }
  return changed || handsMoved;
  }

bool MdlVisual::hasGaze() const {
  return Gaze::isEnabled() && skInst->gazeIk().isValid();
  }

bool MdlVisual::hasQuadrupedIk() const {
  return FootIk::isEnabled() && skInst->footIk().isQuadruped();
  }

bool MdlVisual::updateFootIk(Npc& npc, World& world, uint64_t dt, bool alignedToGround) {
  if(npc.usesSimplifiedMovement())
    return false;
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
    pushBodyPose();
  return changed;
  }

void MdlVisual::processLayers(World& world, NpcLogicalState* logic) {
  Pose&    pose      = *skInst;
  uint64_t tickCount = world.tickCount();
  pose.processLayers(solver,tickCount,logic);
  }

bool MdlVisual::processEvents(World& world, uint64_t &barrier, Animation::EvCount &ev, NpcLogicalState* logic) {
  Pose&    pose      = *skInst;
  uint64_t tickCount = world.tickCount();
  return pose.processEvents(barrier,tickCount,ev,logic);
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

Vec3 MdlVisual::mapHeadBone() const {
  if(skeleton->BIP01_HEAD==size_t(-1))
    return {pos.at(3,0), pos.at(3,1)+180, pos.at(3,2)};
  return mapBone(skeleton->BIP01_HEAD);
  }

bool MdlVisual::eyesInHead(Vec3& at) const {
  const auto* e = face.eyes();
  if(e==nullptr || skeleton==nullptr || head.boneId!=skeleton->BIP01_HEAD)
    return false;
  // the head bone: x up, y across, z forward (the frame of a head's vertices)
  at = (e->eye[0].center + e->eye[1].center)*0.5f;
  return true;
  }

Vec3 MdlVisual::mapEyes() const {
  const size_t bone = skeleton->BIP01_HEAD;
  if(bone==size_t(-1) || bone>=skInst->boneCount())
    return mapHeadBone();
  Vec3 at = Vec3(EyesUp,0,0);
  eyesInHead(at);
  renderedBone(bone).project(at);
  return at;
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

const Animation::Sequence* MdlVisual::movementSequence(WalkBit mode, WeaponState weapon, const Npc& npc) const {
  return solver.solveAnim(AnimationSolver::Move,weapon,mode,*skInst,&npc);
  }

const Animation::Sequence* MdlVisual::idleSequence(WalkBit mode, WeaponState weapon, const Npc& npc) const {
  return solver.solveAnim(AnimationSolver::Idle,weapon,mode,*skInst,&npc);
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

const Animation::Sequence* MdlVisual::startAnimAndGet(Npc &npc, std::string_view name, uint8_t comb, BodyState bs, bool force) {
  const Animation::Sequence* sq  = solver.solveFrm(name);
  if(skInst->startAnim(solver,sq,comb,bs,force ? Pose::Force : Pose::NoHint,npc.world().tickCount()))
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
      if(npc.isUsingItemInHand() || bool(wlk & WalkBit::WM_Walk))
        bs = BS_WALK; else
        bs = BS_RUN;
      break;
    case AnimationSolver::Anim::MoveBack:
      if(npc.isUsingItemInHand())
        bs = BS_WALK;
      else if(st==WeaponState::Fist || st==WeaponState::W1H || st==WeaponState::W2H)
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
  addTextureOwner(slot.view);
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
  quiv.rebind(to);
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
  if(quiv.isShown() && quiv.bone()<skInst->boneCount())
    quiv.sync(renderedBone(quiv.bone()),
              renderedBone(quiv.spineBone()<skInst->boneCount() ? quiv.spineBone() : quiv.bone()));
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
      p = attachMatrix(torch.boneId);
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
    p = attachMatrix(att.boneId);
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
  const uint16_t count = 11;
  std::vector<uint32_t>                   time(count);
  std::vector<const Animation::AnimData*> seen;
  for(uint16_t i=0; i<count; ++i) {
    auto sq = solver.solveFrm(gestureName(uint16_t(i+1)).data());
    if(sq==nullptr || sq->data==nullptr || std::find(seen.begin(),seen.end(),sq->data.get())!=seen.end())
      continue;
    seen.push_back(sq->data.get());
    time[i] = uint32_t(sq->totalTime());
    }
  gestures.setGestures(std::move(time));
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

  const uint16_t count = 11;
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
  // the head turns elsewhere, or the eyes jump far
  const uint32_t sh  = skInst->gazeIk().shiftCount() + eyes.bigSaccades();

  EyeBlink::Input in;
  in.now      = now;
  in.dt       = dt;
  in.allowed  = !npc.isDown() && (npc.bodyStateMasked() & BS_MAX)!=BS_LIE && faceWeight(npc,"S_EYESCLOSED")<0.1f;
  in.speaking = mouth.isSpeaking(now) || npc.speechLeft()>0;
  in.voice    = mouth.voiceLevel();
  in.shift    = sh!=blinkShift;
  // a frightened face blinks more, a hostile or angry stare less
  in.rate     = 1.f + 0.6f*faceWeight(npc,"S_FRIGHTENED")
                    - 0.3f*std::max(faceWeight(npc,"S_HOSTILE"),faceWeight(npc,"S_ANGRY"));
  blinkShift  = sh;

  uint64_t hold = 0;
  if(blink.update(in,hold)) {
    if(face.isActive())
      face.startAnim("R_EYESBLINK",1,now+hold,now); // a layer of its own: never in place of a face
    else
      head.view.startMMAnim("R_EYESBLINK",1,now+hold,true);  // not in place of a face
    }
  if(probing)
    blink.probe(EyeBlink::subjectProbe);
  }

float MdlVisual::faceWeight(const Npc& npc, std::string_view anim) const {
  if(face.isActive())
    return face.weight(anim,npc.world().tickCount());
  return head.view.morphWeight(anim);
  }

void MdlVisual::startCues(Npc& npc, std::shared_ptr<const SpeechCues::Plan> plan, bool voiced) {
  cues.start(std::move(plan),voiced,npc.world().tickCount());
  }

bool MdlVisual::startCue(Npc& npc, std::string_view name, char side) {
  const auto* profile = face.faceProfile();
  const auto* cue     = profile!=nullptr ? profile->findCue(name) : nullptr;
  if(cue==nullptr)
    return false;
  if(side==0)
    side = SpeechCues::sideOf(SpeechCues::seedOf(name)^(uint32_t(npc.instanceSymbol())*2654435761u)^(cueCount++*40503u));
  const uint64_t now = npc.world().tickCount();
  return playCue(name,cue->side!=0 && side!=cue->side,-1,now,now);
  }

bool MdlVisual::playCue(std::string_view name, bool mirror, float hold, uint64_t at, uint64_t now) {
  if(!face.startCue(name,mirror,hold,at,now))
    return false;
  if(const auto* cue = face.faceProfile()->findCue(name); cue!=nullptr && cue->look.has_value())
    eyes.startCue(*cue,mirror,std::min(at,now));
  return true;
  }

float MdlVisual::cueWeight(const Npc& npc, std::string_view name) const {
  const uint64_t now = npc.world().tickCount();
  return std::max(face.cueWeight(name,now),eyes.cueWeight(name,now));
  }

bool MdlVisual::startLeak(Npc& npc, std::string_view felt, bool lower) {
  const auto* profile = face.faceProfile();
  if(profile==nullptr || profile->findLeak(felt,lower)==nullptr)
    return false;
  // a line of its own without a voice: the leak now (its word 0.1 s on), the liar's blinks in the time after
  auto plan = std::make_shared<SpeechCues::Plan>("leak");
  plan->leak     = SpeechCues::Plan::Leak{std::string(felt),lower,0};
  plan->silentMs = ManualLeakMs;
  plan->seed     = SpeechCues::seedOf(felt)^(uint32_t(npc.instanceSymbol())*2654435761u)^(cueCount++*40503u);
  plan->setWord(0,float(SpeechCues::UpperLeadMs)/1000.f,0.5f);
  cues.start(std::move(plan),false,npc.world().tickCount());
  return true;
  }

float MdlVisual::leakWeight(const Npc& npc) const {
  return face.leakWeight(npc.world().tickCount());
  }

void MdlVisual::updateEyes(Npc& npc, uint64_t now) {
  if(!eyes.isActive() || skInst==nullptr || head.boneId>=skInst->boneCount())
    return;
  Eyes::Input in;
  in.now      = now;
  in.head     = renderedBone(head.boneId);
  in.target   = lookSource!=Gaze::Source::None;
  in.at       = lookAt;
  // Gaze aims the head at one's head bone, the eyes go to its eyes (the bone is at the neck)
  const Npc* whom = nullptr;
  Vec3       at;
  if(in.target && npc.gazeTarget(at,&whom)==lookSource && whom!=nullptr)
    in.at = whom->mapEyes();
  in.talk     = lookSource==Gaze::Source::Talk;
  in.speaking = mouth.isSpeaking(now) || npc.speechLeft()>0;
  in.left     = std::max(npc.speechLeft(),mouth.voiceLeft(now));
  // thinking: the eyes go all the way aside (the head a part of it, updateGaze)
  const auto& as = gestures.eyesAside();
  in.aside    = in.talk && as.weight>=0.5f;
  in.asideAt  = asidePoint(mapEyes(),in.at,as.yaw,as.pitch);
  eyes.update(in);
  face.setEyes(eyes.turn(0),eyes.turn(1));
  }

void MdlVisual::updateFace(Npc& npc) {
  if(!face.isActive()) {
    if(&npc==Eyes::probeNpc)
      Eyes::subjectProbe = Eyes::Probe();
    return;
    }
  const bool     probing = &npc==FaceRig::probeNpc;
  const uint64_t now     = npc.world().tickCount();
  // the gestures of the line, far too: a face seen again shows them as they go
  cueStarts.clear();
  cues.update(*face.faceProfile(),now,blink.lastBlink(),cueStarts);
  for(auto& s:cueStarts.cues)
    playCue(s.name,s.mirror,s.hold,s.at,now);
  // a lie: the leak of the felt emotion, the liar's blinks (ordinary ones keep away from them), a smile put on
  if(cueStarts.leak)
    face.startLeak(cueStarts.leakFelt,cueStarts.leakLower,cueStarts.leakAt,now);
  for(auto r:cueStarts.reserve)
    blink.reserve(r);
  for(uint32_t i=0; i<cueStarts.blinks && EyeBlink::isEnabled(); ++i)
    face.startAnim("R_EYESBLINK",1,now+SpeechCues::LiarBlinkHold,now);
  face.setPosed(cueStarts.posed,now);
  // a far face stays as it was last shown, as a far one does not blink; its eyes stop
  if(!head.view.isEmpty() && (npc.processPolicy()<=NpcProcessPolicy::AiNormal || probing)) {
    updateEyes(npc,now);
    const auto r = face.update(now);
    if(r!=FaceRig::Result::Same) {
      const auto start = std::chrono::steady_clock::now();
      if(r==FaceRig::Result::Deformed)
        head.view.setPartDeformation(face.deformation(),face.changed().first,face.changed().second); else
        head.view.setDeformation({});
      faceUploadUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
      }
    }
  if(probing) {
    face.probe(FaceRig::subjectProbe,now);
    FaceRig::subjectProbe.uploadUs = faceUploadUs;
    }
  if(&npc==Eyes::probeNpc)
    eyes.probe(Eyes::subjectProbe);
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
    if(i==&head && face.isActive()) {
      face.startAnim(anim,1,uint64_t(-1),npc.world().tickCount());
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
  if(face.isActive())
    face.startAnim(anim,intensity,duration,npc.world().tickCount()); else
    head.view.startMMAnim(anim,intensity,duration);
  }

void MdlVisual::startSpeech(Npc& npc, std::shared_ptr<LipSync::Track> voice) {
  if(voice==nullptr)
    return;
  // the mouth shapes of the head: a head without them gets the talk loop
  std::vector<float> shapes;
  if(face.isActive())
    shapes = face.strength("VISEME");
  else if(auto proto = head.view.protoMesh())
    for(auto& m:proto->morph)
      if(m.name=="VISEME") {
        shapes = m.strength;
        break;
        }
  mouth.setShapes(std::move(shapes));
  cues.voice(voice,npc.world().tickCount());
  mouth.start(std::move(voice),npc.world().tickCount());
  }

void MdlVisual::updateMouth(Npc& npc, uint64_t dt) {
  LipSync::Shape s;
  if(mouth.update(npc.world().tickCount(),dt,s)) {
    if(face.isActive())
      face.setShape("VISEME",s.frame0,s.frame1,s.alpha,s.intensity,npc.world().tickCount()); else
      head.view.setMMShape("VISEME",s.frame0,s.frame1,s.alpha,s.intensity);
    }
  if(&npc==LipSync::probeNpc)
    mouth.probe(LipSync::subjectProbe);
  }

void MdlVisual::stopDlgAnim(Npc& npc) {
  mouth.stop();
  cues.stop();
  gestures.stop();
  gestureSeq = nullptr;
  const uint16_t count = 11;
  for(uint16_t i=0; i<count; i++)
    skInst->stopAnim(gestureName(uint16_t(i+1)).data());

  if(npc.processPolicy()<=NpcProcessPolicy::AiNormal) {
    // avoid PCI traffic on distant npc's
    startFaceAnim(npc,"VISEME",1,0);
    }
  }
