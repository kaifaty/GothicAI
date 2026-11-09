#include "mdlvisual.h"

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
#include "graphics/mesh/animmath.h"
#include "graphics/mesh/ikmath.h"
#include "frameprofile.h"
#include "worldchunksruntime.h"
#include "utils/workers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>

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
  if(!bodyRig->solve(source,skInst->boneCount(),rigPalette.data(),RenderRig::Hands{rigHold,rigWorld})) {
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
  if(textureOwnerKnown)
    arrow.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
  const auto* residency=world.chunkResidency();
  Resources::TextureLoadScope textureLoadScope(textureOwnerKnown && textureOwnerSpatial && residency &&
      residency->textureDistance(ChunkId::fromPosition(textureOwnerPosition.x,textureOwnerPosition.z))>2);
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
      if(textureOwnerKnown)
        pin.view.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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

void MdlVisual::setVisualBody(WorldView& owner, MeshObjects::Mesh&& body) {
  // Mobsi keep their solver and pose while only these draw instances leave residency.
  attach.clear();
  if(auto proto=body.protoMesh())
    for(const auto& slot:proto->attach)
      if(!slot.hasNode)
        setSlotAttachment(owner.addAtachView(slot,0),slot.name);
  view=std::move(body);
  bodyDetached=false;
  if(textureOwnerKnown)
    view.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
  bindBodyRig();
  pushBodyPose();
  for(auto& slot:item)
    if(slot.deferredProto!=nullptr) {
      slot.view=owner.addView(slot.deferredProto,slot.deferredMaterial);
      if(textureOwnerKnown)
        slot.view.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
  if(textureOwnerKnown)
    view.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
  if(textureOwnerKnown)
    itm.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
  if(textureOwnerKnown)
    itm.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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

void MdlVisual::setQuiver(const Quiver::State& st, MeshObjects::Mesh&& body, std::vector<MeshObjects::Mesh>&& arrows,
                          std::vector<Tempest::Matrix4x4>&& arrowLocal) {
  if(!st.on || skeleton==nullptr) {
    clearQuiver();
    return;
    }
  // the fit is the armour's and the skeleton's: a new count of arrows keeps it (and the swing)
  Quiver::Fit fit = quiv.fit();
  std::vector<size_t>  near    = quiv.near();
  std::vector<uint8_t> nearLeg = quiv.nearLeg();
  std::vector<size_t>  wide    = quiv.wide();
  if(!fit.valid || quiv.state().body!=st.body || quiv.state().skeleton!=st.skeleton) {
    std::vector<Quiver::SkinPoint> bind;
    quiverPoints(true,bind);
    fit = Quiver::fit(*skeleton,bind);
    Quiver::nearPoints(fit,*skeleton,bind,near,nearLeg);
    Quiver::widePoints(fit,*skeleton,bind,near,wide);
    quiverGround = QuiverGround();
    }
  quiv.set(st,fit,std::move(body),std::move(arrows),std::move(arrowLocal));
  quiv.setNear(std::move(near),std::move(nearLeg));
  quiv.setWide(std::move(wide));
  if(textureOwnerKnown)
    quiv.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
  syncAttaches();
  }

void MdlVisual::clearQuiver() {
  quiv.clear();
  }

void MdlVisual::quiverPoints(bool bind, std::vector<Quiver::SkinPoint>& out) const {
  out.clear();
  if(skeleton==nullptr)
    return;
  const size_t n = bind ? skeleton->tr.size() : std::min(skeleton->tr.size(),skInst->boneCount());
  std::vector<Matrix4x4> bones(n);
  for(size_t i=0; i<n; ++i)
    bones[i] = bind ? skeleton->tr[i] : renderedBone(i);
  quiverPoints(bones.data(),n,out);
  }

void MdlVisual::quiverPoints(const Matrix4x4* bones, size_t n, std::vector<Quiver::SkinPoint>& out) const {
  out.clear();
  if(skeleton==nullptr)
    return;
  const ProtoMesh* pm = view.protoMesh();
  if(pm==nullptr)
    return;
  // the skin on a render rig: its bones solved from Gothic's, each point on the Gothic node of its bone
  std::vector<Matrix4x4> rig;
  size_t                 skinN = 0;
  const Matrix4x4*       skin  = skinOf(bones,n,rig,skinN);
  for(const auto& v:pm->skinVertexData()) {
    Quiver::SkinPoint p;
    float weight = 0, best = 0;
    for(size_t r=0; r<4; ++r) {
      if(!(v.weights[r]>0.f) || v.boneId[r]>=skinN)
        continue;
      Vec3 part(v.pos[r][0],v.pos[r][1],v.pos[r][2]);
      skin[v.boneId[r]].project(part);
      p.pos  += part*v.weights[r];
      p.src   = size_t(&v-pm->skinVertexData().data());
      weight += v.weights[r];
      if(v.weights[r]>best) {
        best   = v.weights[r];
        p.bone = bodyRig!=nullptr ? bodyRig->sourceOf(v.boneId[r]) : v.boneId[r];
        }
      }
    if(weight>0.f) {
      p.pos = p.pos/weight;
      out.push_back(p);
      }
    }
  // the armour's rigid parts without a node of their own hang on the bone of their name
  for(size_t i=0; i<pm->attach.size() && i<pm->attachPoints.size(); ++i) {
    if(pm->attach[i].hasNode)
      continue;
    const size_t bone = skeleton->findNode(pm->attach[i].name);
    if(bone>=n)
      continue;
    for(auto pt:pm->attachPoints[i]) {
      bones[bone].project(pt);
      out.push_back({pt,bone});
      }
    }
  }

// the calling thread's CPU time, µs: the quiver's own share of a step, apart from the thread being put aside;
// read only while the bench measures (a call per quiver and frame is not the game's), 0 otherwise
static double threadCpuUs(bool on) {
  if(!on)
    return 0;
  return Workers::threadCpuUs();
  }

bool MdlVisual::updateQuiver(const Npc& npc, uint64_t dt) {
  if(!quiv.isShown() || skeleton==nullptr || quiv.bone()>=skInst->boneCount())
    return false;
  // far or not drawn: at rest, not computed
  const auto* cam = Gothic::inst().camera();
  const Vec3  at  = npc.position();
  if(view.isEmpty() || (cam!=nullptr && (cam->originLwc()-at).length()>Quiver::SimDistance)) {
    quiv.rest();
    return false;
    }
  // the thread's CPU time is read outside the wall time it is set against (its own call is not the quiver's)
  const bool   cpuOn = Quiver::measureGround; // once: the bench may turn it on meanwhile
  const double cpu0  = threadCpuUs(cpuOn);
  const auto   t0   = std::chrono::steady_clock::now();
  quiverRays  = 0;
  quiverRayUs = 0;
  const auto&  sk   = *skeleton;
  const size_t n    = std::min(sk.tr.size(),skInst->boneCount());
  const size_t spineBone = quiv.spineBone()<n ? quiv.spineBone() : quiv.bone();
  const auto&  pelvisNow = renderedBone(quiv.bone());
  // the ground: looked for only when the tube comes near it (the pelvis low: a roll, sneaking, sitting); standing,
  // walking and running it never does. Near: closer than GroundNear, till farther than GroundFar again. Rays down the
  // world's collision round the pelvis and under the tube, again when they have moved
  Quiver::Ground ground;
  {
    Vec3 mid, bottom;
    const float lowest = quiv.lowest(pelvisNow,renderedBone(spineBone),&mid,&bottom);
    const Vec3  hip    = {pelvisNow.at(3,0),pelvisNow.at(3,1),pelvisNow.at(3,2)};
    float       floorY = at.y;
    const bool  cached = quiverGround.valid &&
                         std::hypot(hip.x-quiverGround.at.x,hip.z-quiverGround.at.z)<100.f;
    if(cached && quiverGround.plane.on)
      floorY = std::max(floorY,quiverGround.plane.at.y);
    quiverClear = lowest-floorY;
    quiverNear  = quiverClear<(quiverNear ? Quiver::GroundFar : Quiver::GroundNear);
    if(quiverNear) {
      if(!cached || std::hypot(hip.x-quiverGround.at.x,hip.z-quiverGround.at.z)>Quiver::GroundResample ||
         std::abs(hip.y-quiverGround.at.y)>Quiver::GroundResample ||
         std::hypot(bottom.x-quiverGround.bottom.x,bottom.z-quiverGround.bottom.z)>Quiver::GroundResample)
        {
        const auto r0 = std::chrono::steady_clock::now();
        lookForGround(npc,hip,mid,bottom);
        quiverRayUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-r0).count());
        }
      ground = quiverGround.plane;
      }
  }
  if(ground.on && quiv.restsOnGround(pelvisNow)) {
    quiverStepUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count());
    quiverCpuUs  = float(threadCpuUs(cpuOn)-cpu0);
    return false;
    }
  // the right leg and arm: the hit zones' capsules, fitted to the worn armour
  static thread_local std::vector<HitZones::Posed>     caps;
  static thread_local std::vector<Quiver::Obstacle>    obs;
  static thread_local std::vector<Matrix4x4>           bones;
  obs.clear();
  bones.resize(n);
  for(size_t i=0; i<n; ++i)
    bones[i] = renderedBone(i);
  hitZones.posed(bones.data(),n,1.f,caps);
  for(auto& c:caps) {
    if(c.bone>=sk.nodes.size())
      continue;
    const std::string_view name = sk.nodes[c.bone].name;
    const bool leg = name=="BIP01 R THIGH" || name=="BIP01 R CALF" || name=="BIP01 R FOOT";
    if(leg || name=="BIP01 R FOREARM" || name=="BIP01 R HAND")
      obs.push_back({c.a,c.b,c.r,leg ? Quiver::Leg : Quiver::Arm});
    }
  // the armour's flaps and the legs' skin near the quiver: its points; with the ground near, the far ones too
  if(const ProtoMesh* pm = view.protoMesh()) {
    static thread_local std::vector<Matrix4x4> rig;
    size_t           skinN = 0;
    const Matrix4x4* skin  = skinOf(bones.data(),n,rig,skinN);
    const auto& sv   = pm->skinVertexData();
    const auto& legs = quiv.nearLeg();
    const size_t nNear = quiv.near().size();
    const size_t nAll  = nNear + (ground.on ? quiv.wide().size() : 0);
    for(size_t k=0; k<nAll; ++k) {
      const size_t id = k<nNear ? quiv.near()[k] : quiv.wide()[k-nNear];
      if(id>=sv.size())
        continue;
      const auto& v = sv[id];
      Vec3  p;
      float weight = 0;
      for(size_t r=0; r<4; ++r) {
        if(!(v.weights[r]>0.f) || v.boneId[r]>=skinN)
          continue;
        Vec3 part(v.pos[r][0],v.pos[r][1],v.pos[r][2]);
        skin[v.boneId[r]].project(part);
        p      += part*v.weights[r];
        weight += v.weights[r];
        }
      if(weight>0.f) {
        p = p/weight;
        obs.push_back({p,p,0.f,k>=nNear ? Quiver::Body : (k<legs.size() && legs[k]!=0 ? Quiver::Leg : Quiver::Flap)});
        }
      }
    }
  // the bow and a two-hander on the back
  for(auto* att:{&bow,&sword})
    for(auto& p:carriedPoints(*att,att==&bow ? "ZS_BOW" : "ZS_LONGSWORD"))
      obs.push_back({p,p,0.f,Quiver::Carried});
  const size_t spine = spineBone;
  quiv.step(bones[quiv.bone()],bones[spine],obs,float(dt)*0.001f,ground);
  quiverStepUs = float(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count());
  quiverCpuUs  = float(threadCpuUs(cpuOn)-cpu0);
  return true;
  }

void MdlVisual::lookForGround(const Npc& npc, const Vec3& hip, const Vec3& mid, const Vec3& bottom) {
  // rays down round the pelvis (three, 30 cm off) and under the tube (its middle and bottom now): a plane fitted
  // to where they hit, raised to the highest of them so no hit is over it; fewer than three: level at the highest
  quiverGround        = QuiverGround();
  quiverGround.valid  = true;
  quiverGround.at     = hip;
  quiverGround.bottom = bottom;
  const auto* phys = npc.world().physic();
  if(phys==nullptr)
    return;
  Vec3 hit[5];
  int  hits = 0;
  Vec3 from[5];
  for(int i=0; i<3; ++i) {
    const float a = float(i)*2.f*float(M_PI)/3.f;
    from[i] = {hip.x+30.f*std::cos(a),hip.y,hip.z+30.f*std::sin(a)};
    }
  from[3] = mid;
  from[4] = bottom;
  for(auto& p:from) {
    ++quiverRays;
    const float top = std::max(p.y,hip.y);
    const auto  r   = phys->rayStatic(Vec3(p.x,top+60.f,p.z),Vec3(p.x,top-150.f,p.z));
    if(r.hasCol)
      hit[hits++] = r.v;
    }
  if(hits==0)
    return;
  auto& g = quiverGround.plane;
  g.on = true;
  g.n  = {0,1,0};
  g.at = hit[0];
  for(int i=1; i<hits; ++i)
    if(hit[i].y>g.at.y)
      g.at = hit[i];
  if(hits<3)
    return;
  // y = a x + b z + c by least squares about the mean
  Vec3 mean;
  for(int i=0; i<hits; ++i)
    mean += hit[i];
  mean = mean/float(hits);
  double sxx=0, sxz=0, szz=0, sxy=0, szy=0;
  for(int i=0; i<hits; ++i) {
    const double x = hit[i].x-mean.x, z = hit[i].z-mean.z, y = hit[i].y-mean.y;
    sxx += x*x; sxz += x*z; szz += z*z; sxy += x*y; szy += z*y;
    }
  const double det = sxx*szz-sxz*sxz;
  if(std::abs(det)<1e-3)
    return;
  const float a = float((sxy*szz-szy*sxz)/det), b = float((szy*sxx-sxy*sxz)/det);
  const Vec3  n = Vec3::normalize(Vec3(-a,1.f,-b));
  if(n.y<0.5f)
    return;
  float up = 0;
  for(int i=0; i<hits; ++i)
    up = std::max(up,Vec3::dotProduct(hit[i]-mean,n));
  g.n  = n;
  g.at = mean + n*up;
  }

std::vector<Vec3> MdlVisual::carriedPoints(const MeshAttach& att, std::string_view slot) const {
  // the points of a thing on the back (the bow, a two-hander) in the world; a bow, a morph mesh, by its bounds:
  // a bent stave
  std::vector<Vec3> ret;
  const ProtoMesh* pm = att.view.protoMesh();
  if(pm==nullptr || att.bone!=slot || att.boneId>=skInst->boneCount())
    return ret;
  const auto& m = renderedBone(att.boneId);
  if(!pm->points.empty()) {
    for(size_t i=0; i<pm->points.size(); i+=8) {
      auto p = pm->points[i];
      m.project(p);
      ret.push_back(p);
      }
    return ret;
    }
  const Vec3* b    = pm->bbox();
  const float half = std::max(std::abs(b[0].x),std::abs(b[1].x));
  for(int i=0; i<=20; ++i) {
    const float x = -half + 2.f*half*float(i)/20.f;
    Vec3 p = {x,0,b[1].z-(b[1].z-b[0].z)*(x/half)*(x/half)};
    m.project(p);
    ret.push_back(p);
    }
  return ret;
  }

Quiver::Probe MdlVisual::quiverProbe() const {
  if(!quiv.isShown() || skeleton==nullptr || quiv.bone()>=skInst->boneCount())
    return {};
  std::vector<Quiver::SkinPoint> posed;
  quiverPoints(false,posed);
  std::vector<bool> leftSide(skeleton->nodes.size(),false), trunk(skeleton->nodes.size(),false);
  for(size_t i=0; i<skeleton->nodes.size(); ++i) {
    const auto& name = skeleton->nodes[i].name;
    leftSide[i] = name.find(" L ")!=std::string::npos;
    trunk[i]    = name=="BIP01" || name.find("PELVIS")!=std::string::npos || name.find("SPINE")!=std::string::npos;
    }
  const size_t spine = quiv.spineBone()<skInst->boneCount() ? quiv.spineBone() : quiv.bone();
  return quiv.probe(renderedBone(quiv.bone()),renderedBone(spine),posed,Quiver::legBones(*skeleton),
                    Quiver::armBones(*skeleton),carriedPoints(bow,"ZS_BOW"),carriedPoints(sword,"ZS_LONGSWORD"),
                    leftSide,trunk);
  }

std::vector<Tempest::Vec3> MdlVisual::quiverSurface(int rings, int around) const {
  if(!quiv.isShown() || skeleton==nullptr || quiv.bone()>=skInst->boneCount())
    return {};
  const size_t spine = quiv.spineBone()<skInst->boneCount() ? quiv.spineBone() : quiv.bone();
  return quiv.surface(renderedBone(quiv.bone()),renderedBone(spine),rings,around);
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

void MdlVisual::setTextureOwner(const Vec3& position, bool spatial) {
  if(textureOwnerKnown && textureOwnerSpatial==spatial &&
     (!spatial || ChunkId::fromPosition(textureOwnerPosition.x,textureOwnerPosition.z)==ChunkId::fromPosition(position.x,position.z)))
    return;
  textureOwnerKnown=true;
  textureOwnerSpatial=spatial;
  textureOwnerPosition=position;
  view.setTextureOwner(position,spatial);
  MeshAttach* slots[]={&head,&sword,&shield,&bow,&ammunition,&stateItm};
  for(auto* slot:slots)
    slot->view.setTextureOwner(position,spatial);
  for(auto& slot:item)
    slot.view.setTextureOwner(position,spatial);
  for(auto& slot:attach)
    slot.view.setTextureOwner(position,spatial);
  for(auto& pin:stuckArrows)
    pin.view.setTextureOwner(position,spatial);
  quiv.setTextureOwner(position,spatial);
  if(torch.view)
    torch.view->setTextureOwner(position,spatial);
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
  if(textureOwnerKnown)
    torch.view->setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
    pushBodyPose();
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
    pushBodyPose();
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
        in.shake  = Tempest::Vec2(npc.manualAimShake().x,npc.manualAimShake().y);
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
  bd.span   = bowSpan();
  // straining, the drawing hand loses ground along the arrow and wins it back (seen only: the arrow leaves
  // drawn as far as Archer says)
  bd.draw   = std::max(0.f,ar.draw() - (ar.stage()==Archer::Drawing || ar.stage()==Archer::Held ?
                                        npc.manualAimShake().z/std::max(bd.span,1.f) : 0.f));
  bd.arrow  = !ammunition.view.isEmpty() && ammunition.bone=="ZS_RIGHTHAND";
  bd.cant   = npc.aimCant();
  bd.kick   = ar.releaseKick();
  bd.settle = ar.releaseSettle();
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
      // as hard as the shot: a heavy bow drawn fully rings, a light one drawn short less
      bow.view.startMMAnim(held>0.5f ? BowDraw::StringLoose : BowLoose,BowDraw::looseIntensity(ar.releaseKick()),uint64_t(-1));
      looseAt = npc.world().tickCount();
      }
    stringByHand = false;
    }
  if(npc.isPlayer())
    pose.bowDrawIk().probe(BowDraw::playerProbe);

  if(changed)
    pushBodyPose();
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
    pushBodyPose();
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
  // where the arrow is for the aim: the follow through after the shot moves the hand only to be seen
  // (graphics/mesh/bowdraw), the aim goes on from where the hand would be without it
  p = Vec3(m.at(3,0),m.at(3,1),m.at(3,2)) - skInst->bowDrawIk().seenOnly();
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

// the bones of the model at a frame of the animation (f past its end: the bind pose), in the model's frame
static void modelFrame(const Skeleton& skeleton, const Animation::AnimData& d, uint32_t f,
                       std::vector<Tempest::Matrix4x4>& world) {
  auto& nodes = skeleton.nodes;
  std::vector<Tempest::Matrix4x4> local(nodes.size());
  world.resize(nodes.size());
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
  std::vector<Tempest::Matrix4x4> world;
  auto yaw = [&](const Tempest::Matrix4x4* m) {
    const float dx = m[lt].at(3,0)-m[rt].at(3,0);
    const float dz = m[lt].at(3,2)-m[rt].at(3,2);
    return std::atan2(dz,dx)*180.f/float(M_PI);
    };
  auto pose = [&](uint32_t f) { modelFrame(*skeleton,d,f,world); };
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

std::string MdlVisual::bladePath(std::string_view name, float length) const {
  // the blade's tip (-length along the hand's x, as the weapon meshes lie) at each frame, in the model's
  // frame, and how fast it goes; the root bone, whose x and z the game keeps at the actor's position (the
  // actor goes the root's way instead); the frames of the fight tags as the MDS gives them
  const Animation::Sequence* sq = solver.solveFrm(name);
  if(sq==nullptr || sq->data==nullptr || skeleton==nullptr)
    return std::string(name) + ": none";
  const auto& d = *sq->data;
  const size_t hand = skeleton->findNode("ZS_RIGHTHAND");
  const size_t root = skeleton->findRootNode();
  if(hand>=skeleton->nodes.size() || root>=skeleton->nodes.size() || d.numFrames==0)
    return std::string(name) + ": no hand";
  char buf[256];
  std::snprintf(buf,sizeof(buf),"%s: frames %u..%u (%u) fps %.1f reverse %d",sq->name.c_str(),unsigned(d.firstFrame),
                unsigned(d.lastFrame),unsigned(d.numFrames),double(d.fpsRate),int(sq->reverse));
  std::string out = buf;
  for(const auto& e:d.events) {
    const char* tag = e.type==asset::MdsEventType::OPTIMAL_FRAME ? "optimal" :
                      e.type==asset::MdsEventType::HIT_END       ? "hit_end" :
                      e.type==asset::MdsEventType::COMBO_WINDOW  ? "window"  : nullptr;
    if(tag==nullptr)
      continue;
    out += std::string("; ") + tag;
    for(auto f:e.frames)
      out += " " + std::to_string(f);
    }
  std::vector<Tempest::Matrix4x4> world;
  Tempest::Vec3 previous;
  for(uint32_t f=0; f<d.numFrames; ++f) {
    modelFrame(*skeleton,d,f,world);
    Tempest::Vec3 tip(-length,0,0), grip;
    world[hand].project(tip);
    world[hand].project(grip);
    const float speed = f>0 ? (tip-previous).length()*d.fpsRate : 0.f;
    previous = tip;
    std::snprintf(buf,sizeof(buf),"; %u: tip %.0f %.0f %.0f grip %.0f %.0f %.0f root %.0f %.0f %.0f %.0f cm/s",
                  unsigned(f),double(tip.x),double(tip.y),double(tip.z),double(grip.x),double(grip.y),double(grip.z),
                  double(world[root].at(3,0)),double(world[root].at(3,1)),double(world[root].at(3,2)),double(speed));
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
  if(textureOwnerKnown)
    slot.view.setTextureOwner(textureOwnerPosition,textureOwnerSpatial);
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
  const uint32_t sh  = skInst->gazeIk().shiftCount();

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

void MdlVisual::updateFace(Npc& npc) {
  if(!face.isActive())
    return;
  const bool     probing = &npc==FaceRig::probeNpc;
  const uint64_t now     = npc.world().tickCount();
  // a far face stays as it was last shown, as a far one does not blink
  if(!head.view.isEmpty() && (npc.processPolicy()<=NpcProcessPolicy::AiNormal || probing)) {
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
