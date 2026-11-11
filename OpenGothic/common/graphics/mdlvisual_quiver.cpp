#include "mdlvisual.h"
#include "settings.h"
#include "gameservices.h"
#include "camera.h"
#include "mdlvisual_internal.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/renderrig.h"
#include "game/serialize.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "worldchunksruntime.h"
#include "utils/workers.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace Tempest;
using namespace MdlVisualInternal;

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
  if(!Settings::physicsFlags().arrowSticking)
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
  const auto* cam = GameServices::inst().camera();
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
