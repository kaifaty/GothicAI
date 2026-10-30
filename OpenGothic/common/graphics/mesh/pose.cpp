#include "pose.h"

#include <Tempest/Log>

#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "game/serialize.h"
#include "skeleton.h"
#include "animmath.h"
#include "ikmath.h"

#include <cmath>
#include <stdexcept>

using namespace Tempest;

namespace {

IkMath::Mat3 polarRotation(const Matrix4x4& matrix) {
  // The full local frame can contain shear even when every native body is rigid: differently
  // rotated parts retain the NPC's anisotropic visual scale. Scaled Newton polar iteration
  // yields a proper rotation without replacing that stretch by normalized matrix columns.
  double x[3][3];
  for(int r=0;r<3;++r)
    for(int c=0;c<3;++c)
      x[r][c]=matrix.at(c,r);
  for(int step=0;step<24;++step) {
    const double det=x[0][0]*(x[1][1]*x[2][2]-x[1][2]*x[2][1])-
                     x[0][1]*(x[1][0]*x[2][2]-x[1][2]*x[2][0])+
                     x[0][2]*(x[1][0]*x[2][1]-x[1][1]*x[2][0]);
    if(!std::isfinite(det) || det<=0)
      throw std::runtime_error("Invalid affine ragdoll bone");
    const double inverseTranspose[3][3]={
      {(x[1][1]*x[2][2]-x[1][2]*x[2][1])/det,(x[1][2]*x[2][0]-x[1][0]*x[2][2])/det,(x[1][0]*x[2][1]-x[1][1]*x[2][0])/det},
      {(x[0][2]*x[2][1]-x[0][1]*x[2][2])/det,(x[0][0]*x[2][2]-x[0][2]*x[2][0])/det,(x[0][1]*x[2][0]-x[0][0]*x[2][1])/det},
      {(x[0][1]*x[1][2]-x[0][2]*x[1][1])/det,(x[0][2]*x[1][0]-x[0][0]*x[1][2])/det,(x[0][0]*x[1][1]-x[0][1]*x[1][0])/det}
      };
    double norm=0,inverseNorm=0;
    for(int r=0;r<3;++r)
      for(int c=0;c<3;++c) {
        norm+=x[r][c]*x[r][c]; inverseNorm+=inverseTranspose[r][c]*inverseTranspose[r][c];
        }
    const double scale=std::sqrt(std::sqrt(inverseNorm/norm));
    double change=0;
    for(int r=0;r<3;++r)
      for(int c=0;c<3;++c) {
        const double next=.5*(scale*x[r][c]+inverseTranspose[r][c]/scale);
        change=std::max(change,std::abs(next-x[r][c])); x[r][c]=next;
        }
    if(change<1e-12)
      break;
    }
  IkMath::Mat3 result;
  for(int r=0;r<3;++r)
    for(int c=0;c<3;++c)
      result.m[r][c]=float(x[r][c]);
  return result;
  }

asset::AnimationSample physicalSample(const Matrix4x4& local) {
  asset::AnimationSample out;
  const auto p = IkMath::position(local);
  // MAN samples use the conjugate of the conventional column-vector rotation: mkMatrix writes
  // their rotation transposed. Use the existing post-pass conversion, then undo that convention.
  const auto q = IkMath::toQuat(IkMath::transpose(polarRotation(local)));
  const float length=std::sqrt(q.w*q.w+q.x*q.x+q.y*q.y+q.z*q.z);
  out.position = zenkit::Vec3(p.x,p.y,p.z);
  out.rotation = zenkit::Quat(q.w/length,q.x/length,q.y/length,q.z/length);
  return out;
  }

Matrix4x4 physicalStretch(const Matrix4x4& local, const asset::AnimationSample& sample) {
  const auto rigid=mkMatrix(sample);
  double h[3][3] = {};
  for(int c=0;c<3;++c)
    for(int r=0;r<3;++r)
      for(int k=0;k<3;++k)
        h[r][c]+=double(rigid.at(r,k))*double(local.at(c,k));
  auto stretch=Matrix4x4::mkIdentity();
  for(int c=0;c<3;++c)
    for(int r=0;r<3;++r)
      stretch.set(c,r,float(.5*(h[r][c]+h[c][r])));
  return stretch;
  }

bool validStretch(const Matrix4x4& m) {
  for(int c=0;c<4;++c)
    for(int r=0;r<4;++r)
      if(!std::isfinite(m.at(c,r)) || std::abs(m.at(c,r))>1e6f ||
         ((c==3 || r==3) && m.at(c,r)!=(c==3 && r==3 ? 1.f : 0.f)))
        return false;
  for(int c=0;c<3;++c)
    for(int r=0;r<3;++r)
      if(m.at(c,r)!=m.at(r,c))
        return false;
  const double a=m.at(0,0),b=m.at(1,0),c=m.at(2,0),d=m.at(1,1),e=m.at(2,1),f=m.at(2,2);
  return a>0 && a*d-b*b>0 && a*(d*f-e*e)-b*(b*f-c*e)+c*(b*e-c*d)>0;
  }

bool validPhysicalFrame(const Matrix4x4& m) {
  for(int c=0; c<4; ++c)
    for(int r=0; r<4; ++r)
      if(!std::isfinite(m.at(c,r)))
        return false;
  const Vec3 x(m.at(0,0),m.at(0,1),m.at(0,2));
  const Vec3 y(m.at(1,0),m.at(1,1),m.at(1,2));
  const Vec3 z(m.at(2,0),m.at(2,1),m.at(2,2));
  return IkMath::dot(x,IkMath::cross(y,z))>0.f &&
         x.length()<=100.001f && y.length()<=100.001f && z.length()<=100.001f &&
         std::abs(m.at(0,3))<=1e-5f && std::abs(m.at(1,3))<=1e-5f &&
         std::abs(m.at(2,3))<=1e-5f && std::abs(m.at(3,3)-1.f)<=1e-5f;
  }

Matrix4x4 relativeAffine(const Matrix4x4& parent, const Matrix4x4& child) {
  const double a=parent.at(0,0), b=parent.at(1,0), c=parent.at(2,0);
  const double d=parent.at(0,1), e=parent.at(1,1), f=parent.at(2,1);
  const double g=parent.at(0,2), h=parent.at(1,2), i=parent.at(2,2);
  const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
  const double inverse[3][3]={
    {(e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det},
    {(f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det},
    {(d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det}
    };
  auto result=Matrix4x4::mkIdentity();
  for(int col=0;col<4;++col)
    for(int row=0;row<3;++row) {
      double value=0;
      for(int k=0;k<3;++k) {
        const double v=col<3 ? double(child.at(col,k)) : double(child.at(3,k))-parent.at(3,k);
        value+=inverse[row][k]*v;
        }
      result.set(col,row,float(value));
      }
  // Both inputs are affine. A general 4x4 inverse rounds its homogeneous element away from one,
  // which would then multiply the physical parent's large world translation on recomposition.
  return result;
  }

}

Pose::Pose() {
  lay.reserve(4);
  }

// Origin replay starts from the already sampled, blended pose. It needs the
// retained procedural state, but never reads animation layers, blend drivers,
// or the physical/recovery histories that a complete Pose copy would allocate.
Pose::Pose(const Pose& source, OriginReplayTag)
  :skeleton(source.skeleton), trY(source.trY), flag(source.flag), lastUpdate(source.lastUpdate),
   headRotX(source.headRotX), headRotY(source.headRotY),
   ik(source.ik), gaze(source.gaze), lean(source.lean), aim(source.aim),
   walk(source.walk), bowDraw(source.bowDraw), flinch(source.flinch),
   numBones(source.numBones), pos(source.pos) {
  // Preserve the full bank: ordered hierarchy construction may read a parent
  // below MAX_NUM_SKELETAL_NODES even when it is outside the current bone count.
  std::copy_n(source.hasSamples,Resources::MAX_NUM_SKELETAL_NODES,hasSamples);
  std::copy_n(source.base,Resources::MAX_NUM_SKELETAL_NODES,base);
  std::copy_n(source.tr,Resources::MAX_NUM_SKELETAL_NODES,tr);
  }

uint8_t Pose::calcAniComb(const Vec3& dpos, float rotation) {
  float   l   = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);

  float   dir = 180.f*std::atan2(dpos.z,dpos.x)/float(M_PI);
  float   aXZ = (rotation-dir);
  float   aY  = -std::atan2(dpos.y,l)*180.f/float(M_PI);

  uint8_t cx  = (aXZ<-30.f) ? 0 : (aXZ<=30.f ? 1 : 2);
  uint8_t cy  = (aY <-45.f) ? 0 : (aY <=45.f ? 1 : 2);

  // sides angle: +/- 30 height angle: +/- 45
  return uint8_t(1u+cy*3u+cx);
  }

uint8_t Pose::calcAniCombVert(const Vec3& dpos) {
  float   l  = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);
  float   aY = 180.f*std::atan2(dpos.y,l)/float(M_PI);
  uint8_t cy = (aY <-25.f) ? 0 : (aY <=25.f ? 1 : 2);

  if(dpos.y<-50)
    cy = 0;
  if(dpos.y>50)
    cy = 2;

  // height angle: +/- 25
  return uint8_t(cy+1u);
  }

void Pose::save(Serialize &fout) {
  uint8_t sz=uint8_t(lay.size());
  fout.write(sz);
  for(auto& i:lay) {
    // the speed is not saved, a load gives the layer its animation's own (a hit stop or setLayerSpeed
    // is lost): write the start that shows the same frame at that speed
    uint64_t    start = i.sAnim;
    const float k     = speedOf(i.seq);
    if(i.speed!=k && lastUpdate>i.sAnim) {
      const double back = double(layerTime(i,lastUpdate)-i.sAnim)/double(k);
      if(back<double(lastUpdate))
        start = lastUpdate-uint64_t(back);
      }
    fout.write(i.seq->name,start,i.bs,uint64_t(0)); // was the start of a blend
    }
  fout.write(lastUpdate);
  fout.write(combo.bits);
  fout.write(rotation ? rotation->name : "");
  fout.write(itemUseSt,itemUseDestSt);
  fout.write(headRotX,headRotY);

  for(auto& i:hasSamples)
    fout.write(uint8_t(i));
  for(auto& i:base)
    fout.write(i);
  for(auto& i:raw)
    fout.write(i);
  for(auto& i:tr)
    fout.write(i);
  }

void Pose::load(Serialize &fin, const AnimationSolver& solver) {
  clearRagdollPose();
  std::string name;
  uint8_t     sz = uint8_t(lay.size());

  fin.read(sz);
  lay.resize(sz);
  for(auto& i:lay) {
    fin.read(name,i.sAnim,i.bs);
    i.sOrigin = i.sAnim;
    if(fin.version()>43) {
      uint64_t sBlend = 0;
      fin.read(sBlend);
      }
    i.seq = solver.solveFrm(name);
    }
  fin.read(lastUpdate);
  fin.read(combo.bits);
  removeIf(lay,[](const Layer& l){
    return l.seq==nullptr;
    });
  fin.read(name);
  for(auto& i:lay) {
    if(i.seq->name==name)
      rotation = i.seq;
    }
  fin.read(itemUseSt,itemUseDestSt);
  hasEvents = 0;
  isFlyCombined = 0;
  hasTransitions = 0;
  for(auto& i:lay)
    onAddLayer(i);
  fin.read(headRotX,headRotY);
  needToUpdate = true;

  numBones = skeleton==nullptr ? 0 : skeleton->nodes.size();
  for(auto& i:hasSamples)
    fin.read(reinterpret_cast<uint8_t&>(i));
  for(auto& i:base)
    fin.read(i);
  for(auto& i:raw)
    fin.read(i);
  for(auto& i:tr)
    fin.read(i);
  blend.reset(numBones);
  }

void Pose::setFlags(Pose::Flags f) {
  flag         = f;
  needToUpdate = true;
  }

BodyState Pose::bodyState() const {
  uint32_t b = BS_NONE;
  for(auto& i:lay)
    b = std::max(b,uint32_t(i.bs&(BS_MAX | BS_FLAG_MASK)));
  return BodyState(b);
  }

bool Pose::hasState(BodyState s) const {
  for(auto& i:lay)
    if(i.bs==s)
      return true;
  return false;
  }

bool Pose::hasStateFlag(BodyState f) const {
  for(auto& i:lay)
    if((i.bs & (BS_FLAG_MASK | BS_MOD_MASK))==f)
      return true;
  return false;
  }

void Pose::setSkeleton(const Skeleton* sk) {
  if(skeleton==sk)
    return;
  clearRagdollPose();
  skeleton = sk;
  for(auto& i:tr)
    i.identity();
  if(skeleton!=nullptr) {
    numBones = skeleton->tr.size();
    } else {
    numBones = 0;
    }
  for(auto& i:hasSamples)
    i = S_None;
  blend.reset(numBones);
  ik.setSkeleton(skeleton);
  gaze.setSkeleton(skeleton);
  lean.setSkeleton(skeleton);
  aim.setSkeleton(skeleton);
  walk.setSkeleton(skeleton);
  bowDraw.setSkeleton(skeleton);
  flinch.setSkeleton(skeleton);
  trY          = skeleton==nullptr ? 0.f : skeleton->rootTr.y;
  needToUpdate = true;
  if(lay.size()>0) //TODO
    Log::d("WARNING: ",__func__," animation adjustment is not implemented");
  lay.clear();

  if(skeleton!=nullptr)
    mkSkeleton(Matrix4x4::mkIdentity());
  }

bool Pose::startAnim(const AnimationSolver& solver, const Animation::Sequence *sq, uint8_t comb, BodyState bs,
                     StartHint hint, uint64_t tickCount) {
  if(sq==nullptr)
    return false;
  // NOTE: zero stands for no-comb, other numbers are comb-index+1
  comb = std::min<uint8_t>(comb, uint8_t(sq->comb.size()));

  const bool force = (hint&Force);

  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      const bool hasNext   = !i.seq->next.empty();
      const bool finished  = i.seq->isFinished(layerTime(i,tickCount),i.sAnim,combo.len()) && !hasNext && (i.seq->animCls!=Animation::Loop);
      const bool interrupt = force || i.seq->canInterrupt(layerTime(i,tickCount),i.sAnim,combo.len());
      if(i.seq==sq && i.comb==comb && !finished) {
        i.bs = bs;
        return true;
        }
      if(!interrupt && !finished)
        return false;
      if(i.bs==BS_ITEMINTERACT) {
        stopItemStateAnim(solver,tickCount);
        return false;
        }
      //WA: Emulate behaviour of vanilla: lurker skips transition animations when entering the water
      const bool swimFix = (i.sAnim==tickCount && i.bs==BS_SWIM && bs==BS_SWIM);
      const Animation::Sequence* tr=nullptr;
      if(i.seq->shortName!=nullptr && sq->shortName!=nullptr && !swimFix) {
        string_frm tansition("T_",i.seq->shortName,"_2_",sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && sq->shortName!=nullptr && i.bs==BS_STAND) {
        string_frm tansition("T_STAND_2_",sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && i.seq->shortName!=nullptr && bs==BS_STAND) {
        string_frm tansition("T_",i.seq->shortName,"_2_STAND");
        tr = solver.solveFrm(tansition);
        bs = tr ? i.bs : bs;
        }
      onRemoveLayer(i);
      i.seq      = tr ? tr : sq;
      i.sAnim    = tickCount;
      i.sOrigin  = tickCount;
      i.comb     = comb;
      i.bs       = bs;
      onAddLayer(i);
      return true;
      }
  addLayer(sq,bs,comb,tickCount);
  return true;
  }

bool Pose::stopAnim(std::string_view name) {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    bool rm = (name.empty() || lay[i].seq->name==name);
    if(itemUseSt!=0 && lay[i].bs==BS_ITEMINTERACT)
      rm = false;

    if(!rm) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

bool Pose::stopWalkAnim() {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(lay[i].bs!=BS_RUN && lay[i].bs!=BS_SPRINT && lay[i].bs!=BS_SNEAK && lay[i].bs!=BS_WALK) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

void Pose::interrupt() {
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(BS_FLAG_INTERRUPTABLE & lay[i].bs) {
      onRemoveLayer(lay[i]);
      } else {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      }
    }
  lay.resize(ret);
  }

void Pose::stopAllAnim() {
  for(auto& i:lay)
    onRemoveLayer(i);
  lay.clear();
  }

void Pose::processLayers(AnimationSolver& solver, uint64_t tickCount) {
  if(hasTransitions==0)
    return;

  size_t ret    = 0;
  bool   doSort = false;
  for(size_t i=0; i<lay.size(); ++i) {
    auto& l = lay[i];
    if(l.seq->animCls==Animation::Transition && l.seq->isFinished(layerTime(l,tickCount),l.sAnim,combo.len())) {
      auto next = solveNext(solver,l);
      if(next!=l.seq) {
        needToUpdate = true;
        onRemoveLayer(l);

        if(next!=nullptr) {
          if(l.seq==rotation)
            rotation = next;
          doSort  = l.seq->layer!=next->layer;
          auto bs = (l.bs & BS_MAX);
          // WA for:
          // 1. for swampshark, "T_STUMBLEB" has next animationn: "S_FISTRUN"
          // 2. for some of jump-back animnations, also 'next' is not empty
          if(bs==BS_STUMBLE || bs==BS_PARADE)
            l.bs = BS_NONE;
          l.seq   = next;
          l.sAnim = tickCount;
          l.sOrigin = tickCount;
          onAddLayer(l);
          ret++;
          }
        continue;
        }
      }
    if(ret!=i)
      lay[ret] = lay[i];
    ret++;
    }
  lay.resize(ret);

  if(doSort) {
    std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
      return a.seq->layer<b.seq->layer;
      });
    }
  }

bool Pose::update(uint64_t tickCount, bool force) {
  if(stopUntil!=0 && tickCount>=stopUntil)
    endHitStop();
  if(lay.size()==0) {
    const bool ret = needToUpdate || !physical.empty() || physicalReturning;
    const bool make = ret || lastUpdate==0;
    if(physicalReturning)
      lastUpdate = tickCount;
    if(make)
      mkSkeleton(pos);
    needToUpdate = false;
    lastUpdate   = tickCount;
    return ret;
    }

  bool needMkSkeleton = false;
  if(lastUpdate!=tickCount || force) {
    for(auto& i:lay) {
      const Animation::Sequence* seq = i.seq;
      if(0<i.comb && i.comb<=i.seq->comb.size()) {
        if(auto sx = i.seq->comb[size_t(i.comb-1)])
          seq = sx;
        }

      auto&        d         = *seq->data;
      const size_t numFrames = d.numFrames;
      if(numFrames==1 && !needToUpdate)
        continue; //mobsi

      needMkSkeleton |= updateFrame(*seq,i.bs,i.sAnim,i.sOrigin,layerTime(i,tickCount));
      }
    // The physical return already owns the transition from the shown pose. A second ordinary
    // offset underneath it would change its target, and is not part of the legacy pose save.
    if(physicalReturning)
      std::copy_n(raw,numBones,base);
    else
      needMkSkeleton |= blend.apply(raw,base,numBones,tickCount);
    lastUpdate   = tickCount;
    needToUpdate = needMkSkeleton;
    }

  if(needMkSkeleton || force || !physical.empty() || physicalReturning) {
    mkSkeleton(pos);
    return true;
    }
  return false;
  }

bool Pose::updateFrame(const Animation::Sequence &s, BodyState bs, uint64_t sTime, uint64_t sOrigin, uint64_t now) {
  auto&        d         = *s.data;
  const size_t idSize    = d.nodeIndex.size();
  const size_t numFrames = d.numFrames;
  if(numFrames==0 || idSize==0 || d.samples.size()%idSize!=0)
    return false; // error

  now = now-sTime;

  float    fpsRate = d.fpsRate;
  uint64_t frame   = uint64_t(float(now)*fpsRate);
  uint64_t frameA  = frame/1000;
  uint64_t frameB  = frame/1000+1; //next

  float    a       = float(frame%1000)/1000.f;

  if(s.animCls==Animation::Loop) {
    frameA%=d.numFrames;
    frameB%=d.numFrames;
    } else {
    frameA = std::min<uint64_t>(frameA,d.numFrames-1);
    frameB = std::min<uint64_t>(frameB,d.numFrames-1);
    }

  if(s.reverse) {
    frameA = d.numFrames-1-frameA;
    frameB = d.numFrames-1-frameB;
    }

  auto* sampleA = &d.samples[size_t(frameA*idSize)];
  auto* sampleB = &d.samples[size_t(frameB*idSize)];

  for(size_t i=0; i<idSize; ++i) {
    size_t idx = d.nodeIndex[i];
    if(idx>=numBones)
      continue;
    auto smp = mix(sampleA[i],sampleB[i],a);
    if(i==0) {
      if(bs==BS_CLIMB)
        smp.position.y = trY;
      else if(bs==BS_SWIM || bs==BS_DIVE)
        smp.position.y = trY;
      else if(bs==BS_JUMP) //else if(s.isFly())
        smp.position.y = trY;
      }

    hasSamples[idx] = S_Valid;
    raw       [idx] = smp;
    blend.drive(idx,s,sOrigin,sampleA[i],sampleB[i],fpsRate);
    }
  return true;
  }

void Pose::mkSkeleton(const Tempest::Matrix4x4& mt) {
  mkSkeleton(mt,mt.at(3,1));
  }

void Pose::mkSkeleton(const Tempest::Matrix4x4& mt, float ikFloorY) {
  if(skeleton==nullptr)
    return;
  Matrix4x4 m = mt;
  m.translate(mkBaseTranslation());
  if(skeleton->ordered)
    implMkSkeleton(m); else
    implMkSkeleton(m,size_t(-1));
  walk.apply(tr,numBones,base,m);
  lean.apply(tr,numBones,Vec3(mt.at(3,0),mt.at(3,1),mt.at(3,2)));
  ik.apply(tr,numBones,ikFloorY,aim.pelvisOffset(),&walk);
  walk.captureFeet(tr,numBones);
  bowDraw.hold(tr,numBones,mt);
  aim.apply(tr,numBones,Vec3(mt.at(3,0),mt.at(3,1),mt.at(3,2)),
            Vec3(mt.at(2,0),0,mt.at(2,2)));
  bowDraw.apply(tr,numBones);
  gaze.apply(tr,numBones);
  flinch.apply(tr,numBones);
  if(!physical.empty())
    applyRagdollPose();
  else if(physicalReturning)
    applyRagdollReturn();
  aim.finish(tr,numBones,Vec3(mt.at(3,0),mt.at(3,1),mt.at(3,2)),
             Vec3(mt.at(2,0),0,mt.at(2,2)));
  }

bool Pose::setRagdollPose(const Skeleton* expected, const std::vector<Matrix4x4>& worldBones,
                        const std::vector<size_t>& physicalBones, float weight) {
  if(expected==nullptr || expected!=skeleton || worldBones.size()!=numBones || physicalBones.empty() ||
     !std::isfinite(weight)) {
    clearRagdollPose();
    return false;
    }
  for(const auto i:physicalBones)
    if(i>=numBones || !validPhysicalFrame(worldBones[i])) {
      clearRagdollPose();
      return false;
      }
  if(weight<=0.f) {
    clearRagdollPose();
    return true;
    }
  physical = worldBones;
  physicalMask.assign(numBones,0);
  for(const auto i:physicalBones)
    physicalMask[i] = 1;
  physicalWeight = std::min(weight,1.f);
  physicalReturning = false;
  physicalRaw.clear();
  physicalShown.clear();
  physicalTargetNext.clear();
  physicalStretchFrom.clear();
  physicalStretchTarget.clear();
  needToUpdate = true;
  return true;
  }

void Pose::clearRagdollPose(bool blendFromPhysical, uint64_t tickCount) {
  if(blendFromPhysical && !physical.empty() && skeleton!=nullptr && numBones>0) {
    physicalShown.resize(numBones);
    physicalRaw.resize(numBones);
    physicalStretchFrom.resize(numBones);
    physicalStretchTarget.resize(numBones);
    for(size_t i=0; i<numBones; ++i) {
      const auto parent = skeleton->nodes[i].parent;
      Matrix4x4 inv = parent<numBones ? tr[parent] : pos;
      inv.inverse();
      const auto local=inv*tr[i];
      physicalShown[i] = physicalSample(local);
      physicalStretchFrom[i] = physicalStretch(local,physicalShown[i]);
      }
    physicalBlend.reset(numBones);
    std::vector<asset::AnimationSample> previous;
    uint64_t previousTime=0;
    if(physicalPreviousWorld.size()==numBones && physicalCurrentTime>physicalPreviousTime &&
       physicalCurrentTime-physicalPreviousTime<=250 && tickCount>=physicalCurrentTime-physicalPreviousTime) {
      previous.resize(numBones);
      for(size_t i=0;i<numBones;++i) {
        const auto parent=skeleton->nodes[i].parent;
        // Both root samples use the new object frame: rebasing the NPC must not inject velocity.
        Matrix4x4 inv=parent<numBones ? physicalPreviousWorld[parent] : pos;
        inv.inverse();
        previous[i]=physicalSample(inv*physicalPreviousWorld[i]);
        }
      previousTime=tickCount-(physicalCurrentTime-physicalPreviousTime);
      }
    physicalBlend.transitionFrom(physicalShown.data(),numBones,tickCount,
                                 previous.empty() ? nullptr : previous.data(),previousTime);
    blend.reset(numBones);
    physicalReturning = true;
    } else {
    physicalBlend.reset(0);
    physicalReturning = false;
    physicalRaw.clear();
    physicalShown.clear();
    physicalTargetNext.clear();
    physicalStretchFrom.clear();
    physicalStretchTarget.clear();
    }
  physical.clear();
  physicalMask.clear();
  physicalPreviousWorld.clear();
  physicalCurrentWorld.clear();
  physicalPreviousTime=0;
  physicalCurrentTime=0;
  needToUpdate = true;
  }

void Pose::saveRagdollRecovery(Serialize& fout) const {
  fout.write(physicalReturning);
  if(!physicalReturning)
    return;
  fout.write(uint16_t(3),skeleton->name(),uint32_t(numBones));
  for(const auto& i:physicalShown)
    fout.write(i);
  for(const auto& i:physicalStretchFrom)
    fout.write(i);
  physicalBlend.saveExternal(fout);
  }

void Pose::loadRagdollRecovery(Serialize& fin) {
  clearRagdollPose();
  bool returning = false;
  fin.read(returning);
  if(!returning)
    return;
  std::string name;
  uint32_t count = 0;
  uint16_t version=0;
  uint32_t nameSize=0;
  fin.read(version,nameSize);
  if((version!=2 && version!=3) || nameSize==0 || nameSize>512)
    throw std::runtime_error("Unsupported ragdoll return entry");
  name.resize(nameSize);
  fin.readBytes(name.data(),name.size());
  fin.read(count);
  if(skeleton==nullptr || name!=skeleton->name() || count!=numBones || count==0)
    throw std::runtime_error("Ragdoll return skeleton mismatch");
  physicalShown.resize(count);
  physicalRaw.resize(count);
  physicalStretchFrom.resize(count);
  physicalStretchTarget.resize(count);
  for(auto& i:physicalShown) {
    fin.read(i);
    if(!validPhysicalFrame(mkMatrix(i)))
      throw std::runtime_error("Invalid ragdoll return pose");
    }
  for(auto& m:physicalStretchFrom) {
    fin.read(m);
    if(!validStretch(m))
      throw std::runtime_error("Invalid ragdoll return stretch");
    }
  physicalBlend.loadExternal(fin,numBones,version>=3);
  blend.reset(numBones);
  physicalReturning = true;
  needToUpdate = true;
  }

void Pose::applyRagdollPose() {
  // Recover finished local relations before adding the large world translation. World-space
  // parent/child matrices have already rounded their attachment positions by that point: even a
  // double inverse cannot recover them. Replay the same procedural state on a private pose at the
  // origin, retaining the object's complete basis and the IK's original absolute ground offset.
  Pose local(*this,OriginReplayTag{});
  auto object=pos;
  object.set(3,0,0.f);
  object.set(3,1,0.f);
  object.set(3,2,0.f);
  local.mkSkeleton(object,pos.at(3,1));
  bool done[Resources::MAX_NUM_SKELETAL_NODES] = {};
  for(size_t i=0; i<numBones; ++i)
    applyRagdollPose(i,local.tr,done);
  if(physicalCurrentWorld.size()==numBones && lastUpdate>physicalCurrentTime) {
    physicalPreviousWorld=physicalCurrentWorld;
    physicalPreviousTime=physicalCurrentTime;
    }
  physicalCurrentWorld.assign(tr,tr+numBones);
  physicalCurrentTime=lastUpdate;
  }

void Pose::applyRagdollPose(size_t i, const Matrix4x4* animated, bool* done) {
  if(done[i])
    return;
  const size_t parent = skeleton->nodes[i].parent;
  if(parent<numBones)
    applyRagdollPose(parent,animated,done);
  if(physicalMask[i]) {
    tr[i] = physicalWeight>=1.f ? physical[i] : IkMath::blend(tr[i],physical[i],physicalWeight);
    } else if(parent<numBones) {
    tr[i] = tr[parent]*relativeAffine(animated[parent],animated[i]);
    }
  done[i] = true;
  }

void Pose::applyRagdollReturn() {
  Matrix4x4 animated[Resources::MAX_NUM_SKELETAL_NODES];
  std::copy_n(tr,numBones,animated);
  for(size_t i=0; i<numBones; ++i) {
    const auto parent = skeleton->nodes[i].parent;
    Matrix4x4 inv = parent<numBones ? tr[parent] : pos;
    inv.inverse();
    const auto local=inv*tr[i];
    physicalRaw[i] = physicalSample(local);
    physicalStretchTarget[i] = physicalStretch(local,physicalRaw[i]);
    }
  if(physicalBlend.hasExternalSeed()) {
    // Sample the same finished animation slightly ahead on a private value copy. This includes
    // root-motion cancellation and post-passes without advancing game state or firing events.
    Pose future=*this;
    future.clearRagdollPose();
    future.blend.reset(numBones);
    future.update(lastUpdate+1,true);
    physicalTargetNext.resize(numBones);
    for(size_t i=0;i<numBones;++i) {
      const auto parent=skeleton->nodes[i].parent;
      Matrix4x4 inv=parent<numBones ? future.tr[parent] : future.pos;
      inv.inverse();
      physicalTargetNext[i]=physicalSample(inv*future.tr[i]);
      }
    physicalBlend.setExternalTarget(physicalRaw.data(),physicalTargetNext.data(),numBones,1000.f);
    }
  physicalReturning = physicalBlend.apply(physicalRaw.data(),physicalShown.data(),numBones,lastUpdate);
  const float affineWeight=physicalBlend.externalWeight(lastUpdate);
  bool done[Resources::MAX_NUM_SKELETAL_NODES] = {};
  auto bone = [&](auto&& self, size_t i) -> void {
    if(done[i])
      return;
    const auto parent = skeleton->nodes[i].parent;
    if(parent<numBones)
      self(self,parent);
    auto stretch=Matrix4x4::mkIdentity();
    for(int c=0;c<3;++c)
      for(int r=0;r<3;++r)
        stretch.set(c,r,physicalStretchTarget[i].at(c,r)*(1.f-affineWeight)+physicalStretchFrom[i].at(c,r)*affineWeight);
    tr[i] = (parent<numBones ? tr[parent] : pos)*(mkMatrix(physicalShown[i])*stretch);
    done[i] = true;
    };
  if(physicalReturning)
    for(size_t i=0; i<numBones; ++i)
      bone(bone,i);
  if(physicalReturning) {
    bool finite=true;
    for(size_t b=0;b<numBones;++b)
      for(size_t i=0;i<16;++i)
        finite &= std::isfinite(tr[b].data()[i]);
    if(!finite) {
      // Individually finite affine locals in a damaged archive can still overflow their hierarchy.
      // Restore the whole finished animation transactionally before any bone reaches rendering.
      std::copy_n(animated,numBones,tr);
      physicalReturning=false;
      }
    }
  if(!physicalReturning) {
    // Record the current animation as the ordinary driver's starting point, so a later switch
    // gets its usual transition without resurrecting an offset from before the body fell.
    blend.apply(raw,base,numBones,lastUpdate);
    physicalBlend.reset(0);
    physicalRaw.clear();
    physicalShown.clear();
    physicalTargetNext.clear();
    physicalStretchFrom.clear();
    physicalStretchTarget.clear();
    }
  }

void Pose::implMkSkeleton(const Matrix4x4 &mt) {
  if(skeleton==nullptr)
    return;
  auto& nodes      = skeleton->nodes;
  auto  BIP01_HEAD = skeleton->BIP01_HEAD;
  for(size_t i=0; i<nodes.size(); ++i) {
    size_t parent = nodes[i].parent;
    auto   mat    = hasSamples[i] ? mkMatrix(base[i]) : nodes[i].tr;

    if(parent<Resources::MAX_NUM_SKELETAL_NODES)
      tr[i] = tr[parent]*mat; else
      tr[i] = mt*mat;

    if(i==BIP01_HEAD && (headRotX!=0 || headRotY!=0)) {
      Matrix4x4& m = tr[i];
      m.rotateOY(headRotY);
      m.rotateOX(headRotX);
      }
    }
  }

void Pose::implMkSkeleton(const Tempest::Matrix4x4 &mt, size_t parent) {
  if(skeleton==nullptr)
    return;
  auto& nodes = skeleton->nodes;
  for(size_t i=0;i<nodes.size();++i){
    if(nodes[i].parent!=parent)
      continue;
    auto mat = hasSamples[i] ? mkMatrix(base[i]) : nodes[i].tr;
    tr[i] = mt*mat;
    implMkSkeleton(tr[i],i);
    }
  }

const Animation::Sequence* Pose::solveNext(const AnimationSolver &solver, const Layer& lay) {
  auto sq = lay.seq;

  if((lay.bs & BS_ITEMINTERACT)==BS_ITEMINTERACT && itemUseSt!=itemUseDestSt) {
    int sA = itemUseSt, sB = itemUseSt, nextState = 0;
    if(itemUseSt<itemUseDestSt) {
      sB++;
      nextState = itemUseSt+1;
      } else {
      sB--;
      nextState = itemUseSt-1;
      }
    char scheme[64]={};
    sq->schemeName(scheme);

    const Animation::Sequence* ret = nullptr;
    if(itemUseSt>itemUseDestSt) {
      string_frm T_ID_SX_2_STAND("T_",scheme,"_S",itemUseSt,"_2_STAND");
      ret = solver.solveFrm(T_ID_SX_2_STAND);
      }

    if(ret==nullptr) {
      string_frm T_ID_Sa_2_Sb("T_",scheme,"_S",sA,"_2_S",sB);
      ret = solver.solveFrm(T_ID_Sa_2_Sb);
      }

    if(ret==nullptr && itemUseDestSt>=0)
      return sq;
    itemUseSt = nextState;
    return ret;
    }

  if(sq==rotation)
    return sq;

  return solver.solveNext(*sq);
  }

void Pose::addLayer(const Animation::Sequence *seq, BodyState bs, uint8_t comb, uint64_t tickCount) {
  if(seq==nullptr)
    return;
  Layer l;
  l.seq   = seq;
  l.sAnim = tickCount;
  l.sOrigin = tickCount;
  l.bs    = bs;
  l.comb  = comb;
  lay.push_back(l);
  onAddLayer(lay.back());
  std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
    return a.seq->layer<b.seq->layer;
    });
  }

void Pose::onAddLayer(Pose::Layer& l) {
  l.speed     = speedOf(l.seq);
  l.stopFrom  = 0; // a new animation is not held by the last one's hit stop
  l.catchFrom = 0;
  if(hasLayerEvents(l))
    hasEvents++;
  if(l.seq->isFly())
    isFlyCombined++;
  if(l.seq->animCls==Animation::Transition)
    hasTransitions++;
  needToUpdate = true;
  }

void Pose::onRemoveLayer(const Pose::Layer &l) {
  if(l.seq==rotation)
    rotation=nullptr;
  if(hasLayerEvents(l))
    hasEvents--;
  if(l.seq->animCls==Animation::Transition)
    hasTransitions--;
  if(l.seq->isFly())
    isFlyCombined--;
  // the layers under it take the bones back (AnimBlend sees the switch): even a still one
  needToUpdate = true;
  }

bool Pose::hasLayerEvents(const Pose::Layer& l) {
  if(l.seq==nullptr)
    return false;
  return
      l.seq->data->events.size()>0 ||
      l.seq->data->mmStartAni.size()>0 ||
      l.seq->data->gfx.size()>0;
  }

void Pose::processSfx(Npc &npc, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processSfx(layerTime(i,lastUpdate),i.sAnim,layerTime(i,tickCount),&npc,nullptr);
  }

void Pose::processSfx(Interactive& mob, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processSfx(layerTime(i,lastUpdate),i.sAnim,layerTime(i,tickCount),nullptr,&mob);
  }

void Pose::processPfx(MdlVisual& visual, World& world, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processPfx(layerTime(i,lastUpdate),i.sAnim,layerTime(i,tickCount),visual,world);
  }

bool Pose::processEvents(uint64_t &barrier, uint64_t now, Animation::EvCount &ev) const {
  if(hasEvents==0) {
    barrier=now;
    return false;
    }

  for(auto& i:lay)
    i.seq->processEvents(layerTime(i,barrier),i.sAnim,layerTime(i,now),ev);
  barrier=now;
  return hasEvents>0;
  }

void Pose::setObjectMatrix(const Tempest::Matrix4x4& obj, bool sync) {
  if(pos==obj)
    return;
  pos = obj;
  if(sync)
    mkSkeleton(pos); else
    needToUpdate = true;
  }

Tempest::Vec3 Pose::animMoveSpeed(uint64_t tickCount, uint64_t dt) const {
  // steps with the bow drawn: the legs' own
  const Tempest::Vec3 steps = walk.move(dt);
  for(size_t i=lay.size(); i>0; ) {
    --i;
    auto& lx = lay[i];
    if(!lx.seq->data->hasMoveTr)
      continue;
    return lx.seq->speed(layerTime(lx,tickCount)-lx.sAnim,uint64_t(float(dt)*lx.speed)) + steps;
    }
  return steps;
  }

bool Pose::isDefParWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefParWindow(layerTime(i,tickCount)-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefWindow(layerTime(i,tickCount)-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefence(uint64_t tickCount) const {
  for(auto& i:lay) {
    if(i.bs==BS_PARADE && i.seq->isDefWindow(layerTime(i,tickCount)-i.sAnim))
      return true;
    }
  return false;
  }

auto Pose::combatLayers(uint64_t tickCount) const -> std::vector<CombatLayer> {
  std::vector<CombatLayer> result;
  result.reserve(lay.size());
  for(const auto& i:lay)
    result.push_back({i.seq,i.sAnim,i.sOrigin,layerTime(i,tickCount),i.speed,i.comb,i.bs});
  return result;
  }

bool Pose::isJumpBack(uint64_t tickCount) const {
  for(auto& i:lay) {
    if(i.bs==BS_PARADE && i.seq->data->defWindow.empty())
      return true;
    if(i.bs==BS_PARADE && i.seq->isDefWindow(layerTime(i,tickCount)-i.sAnim))
      return true;
    }
  return false;
  }

bool Pose::isJumpAnim() const {
  for(auto& i:lay) {
    if(i.bs!=BS_JUMP || i.seq->animCls!=Animation::Transition)
      continue;
    return true;
    }
  return false;
  }

bool Pose::isFlyAnim() const {
  return isFlyCombined>0;
  }

bool Pose::isStanding() const {
  if(lay.size()!=1 || lay[0].seq->animCls==Animation::Transition)
    return false;
  auto& s = *lay[0].seq;
  if(s.isIdle())
    return true;
  // check common idle animations
  return s.name=="S_FISTRUN"  || s.name=="S_MAGRUN"  ||
         s.name=="S_1HRUN"    || s.name=="S_BOWRUN"  ||
         s.name=="S_2HRUN"    || s.name=="S_CBOWRUN" ||
         s.name=="S_RUN"      || s.name=="S_WALK"    ||
         s.name=="S_FISTWALK" || s.name=="S_MAGWALK" ||
         s.name=="S_1HWALK"   || s.name=="S_BOWWALK" ||
         s.name=="S_2HWALK"   || s.name=="S_CBOWWALK";
  }

bool Pose::isPrehit(uint64_t now) const {
  for(auto& i:lay)
    if(i.seq->isPrehit(i.sAnim,layerTime(i,now)))
      return true;
  return false;
  }

bool Pose::isAttackAnim() const {
  for(auto& i:lay)
    if(i.seq->isAttackAnim())
      return true;
  return false;
  }

bool Pose::isIdle() const {
  for(auto& i:lay)
    if(!i.seq->isIdle())
      return false;
  return true;
  }

bool Pose::isInAnim(std::string_view sq) const {
  for(auto& i:lay)
    if(i.seq->name==sq)
      return true;
  return false;
  }

bool Pose::isInAnim(const Animation::Sequence *sq) const {
  for(auto& i:lay)
    if(i.seq==sq)
      return true;
  return false;
  }

bool Pose::hasAnim() const {
  return lay.size()>0;
  }

uint64_t Pose::animationTotalTime() const {
  uint64_t ret=0;
  for(auto& i:lay)
    ret = std::max(ret,uint64_t(i.seq->totalTime()/i.speed));
  return ret;
  }

uint64_t Pose::atkTotalTime() const {
  uint16_t comboLen = combo.len();
  uint64_t ret=0;
  for(auto& i:lay)
    ret = std::max(ret,uint64_t(float(i.seq->atkTotalTime(comboLen))/i.speed));
  return ret;
  }

const Animation::Sequence* Pose::continueCombo(const AnimationSolver &solver, const Animation::Sequence *sq,
                                               BodyState bs, uint64_t tickCount) {
  if(sq==nullptr)
    return nullptr;

  Layer* prev = nullptr;
  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      prev = &i;
      break;
      }

  if(prev==nullptr) {
    combo = ComboState();
    return nullptr;
    }

  auto&    d     = *prev->seq->data;
  uint64_t t     = layerTime(*prev,tickCount)-prev->sAnim;
  size_t   id    = combo.len()*2;

  if(0==d.defWindow.size() || 0==d.defHitEnd.size()) {
    if(!startAnim(solver,sq,prev->comb,bs,Pose::NoHint,tickCount))
      return nullptr;
    combo = ComboState();
    return sq;
    }

  if(sq->data->defHitEnd.size()==0) {
    // hit -> block
    startAnim(solver,sq,prev->comb,bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(id+1>=d.defWindow.size()) {
    combo.setBreak();
    return nullptr;
    }

  if(!(d.defWindow[id+0]<t && t<=d.defWindow[id+1])) {
    if(prev->seq->name==sq->name && sq->data->defHitEnd.size()>0)
      combo.setBreak();
    return nullptr;
    }

  if(combo.isBreak())
    return nullptr;

  if(prev->seq->name!=sq->name) {
    startAnim(solver,sq,prev->comb,bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(combo.len()<d.defHitEnd.size()) {
    // the next blow of the combo is no longer held by the last one's hit stop; it goes on from the
    // hit end at the layer's own pace
    if(prev->stopFrom>0.f)
      prev->speed = prev->stopFrom;
    else if(prev->catchFrom>0.f)
      prev->speed = prev->catchFrom;
    prev->stopFrom = prev->catchFrom = 0;
    const uint64_t back = uint64_t(double(d.defHitEnd[combo.len()])/double(prev->speed));
    prev->sAnim = prev->sOrigin = tickCount - std::min(tickCount,back);
    }
  combo.incLen();
  return prev->seq;
  }

uint16_t Pose::comboLength() const {
  return combo.len();
  }

const Tempest::Matrix4x4 Pose::rootNode() const {
  size_t id = 0;
  if(skeleton->rootNodes.size())
    id = skeleton->rootNodes[0];

  const auto& nodes = skeleton->nodes;
  if(id>=nodes.size())
    return Matrix4x4::mkIdentity();

  auto ret = hasSamples[id] ? mkMatrix(base[id]) : nodes[id].tr;
  return ret;
  }

const Matrix4x4 Pose::rootBone() const {
  size_t id = 0;
  if(skeleton->rootNodes.size())
    id = skeleton->rootNodes[0];
  return tr[id];
  }

const Tempest::Matrix4x4& Pose::bone(size_t id) const {
  return tr[id];
  }

size_t Pose::boneCount() const {
  return numBones;
  }

size_t Pose::findNode(std::string_view b) const {
  if(skeleton!=nullptr)
    return skeleton->findNode(b);
  return size_t(-1);
  }

void Pose::setHeadRotation(float dx, float dz) {
  headRotX = dx;
  headRotY = dz;
  }

Vec2 Pose::headRotation() const {
  return Vec2(headRotX,headRotY);
  }

bool Pose::updateGaze(const Gaze::Input& in) {
  if(!gaze.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

std::string Pose::animNames() const {
  std::string s;
  for(auto& i:lay)
    if(i.seq!=nullptr)
      s += (s.empty() ? "" : " ") + i.seq->name;
  return s;
  }

bool Pose::updateFlinch(const Flinch::Input& in) {
  if(!flinch.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

bool Pose::updateAim(const Aim::Input& in) {
  if(!aim.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

uint64_t Pose::layerTime(const Layer& l, uint64_t t) {
  if(l.speed==1.f || t<=l.sAnim)
    return t;
  return l.sAnim + uint64_t(double(t-l.sAnim)*double(l.speed));
  }

float Pose::speedOf(const Animation::Sequence* seq) const {
  if(seq==nullptr)
    return 1;
  for(auto& i:speeds)
    if(i.first==seq->name)
      return i.second;
  return 1;
  }

void Pose::setAnimSpeed(std::string_view name, float k) {
  for(size_t i=0; i<speeds.size(); ++i)
    if(speeds[i].first==name) {
      if(k==1.f)
        speeds.erase(speeds.begin()+ptrdiff_t(i)); else
        speeds[i].second = k;
      return;
      }
  if(k!=1.f)
    speeds.emplace_back(std::string(name),k);
  }

void Pose::setLayerSpeed(const Animation::Sequence* seq, float k, uint64_t now) {
  for(auto& l:lay)
    if(l.seq==seq && now>l.sAnim && k>0.f) {
      const double played = double(layerTime(l,now)-l.sAnim);
      l.sAnim = now-uint64_t(played/double(k));
      l.speed = k;
      l.catchFrom = 0; // a speed set on purpose; a hit stop under way still keeps it (endHitStop)
      }
  }

bool Pose::rebaseLayer(Layer& l, float k, uint64_t at) {
  if(k<=0.f)
    return false;
  if(at<=l.sAnim) {
    l.speed = k;
    return true;
    }
  const double played = double(layerTime(l,at)-l.sAnim);
  const double back   = played/double(k);
  if(back>=double(at))
    return false; // the start would fall before the game's: the layer keeps its pace
  l.sAnim = at-uint64_t(back);
  l.speed = k;
  return true;
  }

void Pose::hitStop(uint64_t until, float k) {
  // from the last shown frame: the time up to it is played and its events are given, none twice
  for(auto& l:lay) {
    if(l.stopFrom==0.f) {
      // one still making up an earlier stop keeps that one's schedule
      l.stopFrom = l.catchFrom>0.f ? l.catchFrom : l.speed;
      if(l.catchFrom==0.f)
        l.dueStart = l.sAnim;
      }
    if(!rebaseLayer(l,l.stopFrom*k,lastUpdate))
      l.stopFrom = 0;
    }
  stopUntil = std::max(stopUntil,until);
  }

void Pose::endHitStop() {
  for(auto& l:lay) {
    // a speed set meanwhile (setLayerSpeed) stays
    if(l.stopFrom>0.f && l.speed<l.stopFrom) {
      // The hold is a loan: the rest of a one-off animation plays faster and ends when it would have
      // without it (at most 1.5 times as fast), so the fighter's next move comes on time. A held
      // attacker otherwise struck its next blow ~0.2 s late, after the struck one had raised its guard
      const float base = l.stopFrom;
      float       k    = base;
      if(l.seq->animCls!=Animation::Loop) {
        const double played = double(layerTime(l,lastUpdate))-double(l.sAnim);
        const double due    = double(lastUpdate>l.dueStart ? lastUpdate-l.dueStart : 0)*double(base);
        const double end    = double(l.seq->atkTotalTime(combo.len()));
        if(due>played && end>played) {
          const double left = std::max((end-due)/double(base),(end-played)/(1.5*double(base)));
          k = float((end-played)/left);
          }
        }
      if(rebaseLayer(l,k,lastUpdate))
        l.catchFrom = k>base ? base : 0.f;
      }
    l.stopFrom = 0;
    }
  stopUntil = 0;
  }

bool Pose::animRelative(std::string_view name, float at, size_t from, size_t to, Tempest::Matrix4x4& out) const {
  // from==size_t(-1) gives the model frame: the chain then includes the root's own sample
  if(skeleton==nullptr || (from>=skeleton->nodes.size() && from!=size_t(-1)) || to>=skeleton->nodes.size())
    return false;
  const Animation::Sequence* seq = nullptr;
  for(auto& i:lay)
    if(i.seq->name==name)
      seq = i.seq;
  if(seq==nullptr || seq->data==nullptr)
    return false;
  auto&        d      = *seq->data;
  const size_t idSize = d.nodeIndex.size();
  if(d.numFrames==0 || idSize==0 || d.samples.size()<size_t(d.numFrames)*idSize)
    return false;
  const float  f  = std::clamp(at,0.f,1.f)*float(d.numFrames-1);
  const size_t fa = std::min(size_t(f),size_t(d.numFrames-1));
  const size_t fb = std::min(fa+1,size_t(d.numFrames-1));
  const float  a  = f-float(fa);
  auto local = [&](size_t node) {
    for(size_t i=0; i<idSize; ++i)
      if(d.nodeIndex[i]==node)
        return mkMatrix(mix(d.samples[fa*idSize+i],d.samples[fb*idSize+i],a));
    return skeleton->nodes[node].tr;
    };
  Tempest::Matrix4x4 m;
  m.identity();
  for(size_t n=to; n!=from; n=skeleton->nodes[n].parent) {
    if(n>=skeleton->nodes.size())
      return false;
    m = local(n)*m;
    }
  out = m;
  return true;
  }

float Pose::animDuration(std::string_view name) const {
  for(auto& i:lay)
    if(i.seq->name==name)
      return i.seq->totalTime()/1000.f/i.speed;
  return 0;
  }

float Pose::animProgress(std::string_view name, uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->name==name) {
      const float total = i.seq->totalTime();
      if(total<=0.f)
        return 1;
      return std::clamp(float(layerTime(i,tickCount)-i.sAnim)/total, 0.f, 1.f);
      }
  return -1;
  }

bool Pose::updateAimWalk(const AimWalk::Input& in) {
  if(!walk.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

bool Pose::updateBowDraw(const BowDraw::Input& input) {
  // the reload: where the drawing hand takes the arrow (the animation at that moment), relative to the chest
  BowDraw::Input in = input;
  size_t chest = size_t(-1), hand = size_t(-1), elbow = size_t(-1);
  if(in.reload>=0.f)
    in.reloadTime = animDuration(BowDraw::ReloadAnim);
  if(in.reload>=0.f && bowDraw.grabNodes(chest,hand,elbow))
    in.grabKnown = animRelative(BowDraw::ReloadAnim,BowDraw::GrabAt,chest,hand,in.grabHand) &&
                   animRelative(BowDraw::ReloadAnim,BowDraw::GrabAt,chest,elbow,in.grabElbow);
  if(!bowDraw.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

bool Pose::updateLean(const Lean::Input& in) {
  if(!lean.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

bool Pose::updateFootIk(const FootIk::Input& in) {
  if(!ik.update(in))
    return false;
  mkSkeleton(pos);
  return true;
  }

void Pose::setAnimRotate(const AnimationSolver &solver, const Npc &npc, WeaponState fightMode, AnimationSolver::TurnType turn, int dir) {
  const Animation::Sequence *sq = nullptr;
  if(dir==0) {
    if(rotation!=nullptr) {
      if(stopAnim(rotation->name))
        rotation = nullptr;
      }
    return;
    }
  if(bodyState()!=BS_STAND)
    return;

  enum AnimationSolver::Anim ani;
  if(turn==AnimationSolver::TurnType::Whirl) {
    ani = (dir<0)?AnimationSolver::Anim::WhirlL:AnimationSolver::Anim::WhirlR;
    } else {
    ani = (dir<0)?AnimationSolver::Anim::RotL:AnimationSolver::Anim::RotR;
    }

  sq = solver.solveAnim(ani,fightMode,npc.walkMode(),*this);
  if(rotation!=nullptr) {
    if(sq!=nullptr && rotation->name==sq->name)
      return;
    if(!stopAnim(rotation->name))
      return;
    }
  if(sq==nullptr)
    return;
  if(startAnim(solver,sq,0,BS_FLAG_FREEHANDS,Pose::NoHint,npc.world().tickCount())) {
    rotation = sq;
    return;
    }
  }

const Animation::Sequence* Pose::setAnimItem(const AnimationSolver &solver, Npc &npc, std::string_view scheme, int state) {
  string_frm T_ID_STAND_2_S0("T_",scheme,"_STAND_2_S0");
  const Animation::Sequence *sq = solver.solveFrm(T_ID_STAND_2_S0);
  if(startAnim(solver,sq,0,BS_ITEMINTERACT,Pose::NoHint,npc.world().tickCount())) {
    itemUseSt     = 0;
    itemUseDestSt = state;
    return sq;
    }
  return nullptr;
  }

bool Pose::stopItemStateAnim(const AnimationSolver& solver, uint64_t tickCount) {
  if(itemUseSt<0)
    return true;
  itemUseDestSt = -1;
  for(auto& i:lay)
    if(i.bs==BS_ITEMINTERACT) {
      auto next = solveNext(solver,i);
      if(next==nullptr)
        continue;
      onRemoveLayer(i);
      i.seq   = next;
      i.sAnim = tickCount;
      i.sOrigin = tickCount;
      onAddLayer(i);
      }
  return true;
  }

const Matrix4x4* Pose::transform() const {
  return tr;
  }

Pose::Probe Pose::playerProbe;
Pose::Probe Pose::subjectProbe;
const void* Pose::probeNpc = nullptr;

void Pose::probe(Probe& p) {
  p.layers.clear();
  for(auto& i:lay) {
    if(!p.layers.empty())
      p.layers += '+';
    p.layers += i.seq->name;
    p.layers += ':' + std::to_string(i.seq->blendIn) + '/' + std::to_string(i.seq->blendOut);
    }
  blend.probe(p.blend);
  p.valid    = skeleton!=nullptr;
  p.skeleton = skeleton;
  p.numBones = skeleton!=nullptr ? std::min(numBones,skeleton->nodes.size()) : 0;
  if(!p.valid)
    return;

  // the bones as the animation has them, without the object matrix, the root moved to x=z=0
  // like mkSkeleton does
  auto&     nodes = skeleton->nodes;
  Matrix4x4 m[Resources::MAX_NUM_SKELETAL_NODES];
  bool      done[Resources::MAX_NUM_SKELETAL_NODES] = {};
  const Matrix4x4 root = rootNode();
  Matrix4x4 org = Matrix4x4::mkIdentity();
  org.translate(Vec3(-root.at(3,0),0,-root.at(3,2)));

  for(size_t i=0; i<p.numBones; ++i) {
    // parents first, whatever the order of the nodes
    size_t chain[Resources::MAX_NUM_SKELETAL_NODES];
    size_t n = 0;
    for(size_t b=i; b<p.numBones && !done[b] && n<p.numBones; b=nodes[b].parent)
      chain[n++] = b;
    while(n>0) {
      const size_t b      = chain[--n];
      const size_t parent = nodes[b].parent;
      const auto   mat    = hasSamples[b] ? mkMatrix(base[b]) : nodes[b].tr;
      m[b]    = (parent<p.numBones ? m[parent] : org)*mat;
      done[b] = true;
      }
    p.joint[i] = Vec3(m[i].at(3,0),m[i].at(3,1),m[i].at(3,2));
    }
  }

Vec3 Pose::mkBaseTranslation() {
  if(numBones==0)
    return Vec3();

  auto  b0 = rootNode();

  float dx = b0.at(3,0);
  float dy = 0;
  float dz = b0.at(3,2);

  if((flag&NoTranslation)==NoTranslation) {
    dy = b0.at(3,1);
    // dy = b0.at(3,1) - translateY();
    }

  return Vec3(-dx,-dy,-dz);
  }

template<class T, class F>
void Pose::removeIf(T &t, F f) {
  size_t ret=0;
  for(size_t i=0;i<t.size();++i) {
    if( !f(t[i]) ) {
      if(ret!=i)
        t[ret] = t[i];
      ret++;
      }
    }
  t.resize(ret);
  }
