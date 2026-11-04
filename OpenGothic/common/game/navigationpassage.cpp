#include "navigationpassage.h"

#include <algorithm>
#include <array>
#include <cmath>

using Tempest::Vec3;

namespace {
// Channels are compared as the learner and the memory teacher see them: uint8.
uint8_t byte(float value) { return uint8_t(std::lround(std::clamp(value,0.f,1.f)*255.f)); }

bool usable(const NavigationMemory::Layer& layer) {
  return layer.size>0 && layer.cellCm>0 && layer.cells.size()==layer.size*layer.size;
  }

// World XZ to the body frame of the view: {right, forward}.
struct Frame final {
  Vec3 origin = {}, forward = {}, right = {};
  bool valid = false;

  explicit Frame(const NavigationMemory::Pose& pose) {
    forward=pose.forward;forward.y=0;
    const float norm=forward.length();
    if(!(norm>1e-3f))
      return;
    forward*=1.f/norm;
    right=Vec3(-forward.z,0,forward.x);
    origin=pose.position;
    valid=true;
    }
  std::array<float,2> local(const Vec3& p) const {
    const auto d=p-origin;
    return {d.x*right.x+d.z*right.z,d.x*forward.x+d.z*forward.z};
    }
  };

// Static hazard and seen free ground within corridorCm of the body-frame point
// (r, f) on the near map. Rotation splats world cells into the body frame, so
// one cell under the line may be empty: the corridor collects its neighbours.
struct NearSample final { bool hazard = false, free = false; };
NearSample nearSample(const NavigationMemory::Layer& near, float r, float f, float corridorCm) {
  const int size=int(near.size);
  const float cell=near.cellCm;
  const int x0=std::max(0,int(std::floor((r-corridorCm)/cell+float(size)*.5f)));
  const int x1=std::min(size-1,int(std::floor((r+corridorCm)/cell+float(size)*.5f)));
  const int z0=std::max(0,int(std::floor((f-corridorCm)/cell+float(size)*.5f)));
  const int z1=std::min(size-1,int(std::floor((f+corridorCm)/cell+float(size)*.5f)));
  NearSample result;
  for(int z=z0;z<=z1;++z)
    for(int x=x0;x<=x1;++x) {
      const auto& c=near.cells[size_t(z)*near.size+size_t(x)];
      const float cr=(float(x)+.5f-float(size)*.5f)*cell,cf=(float(z)+.5f-float(size)*.5f)*cell;
      const float dx=std::max(0.f,std::abs(r-cr)-cell*.5f),dz=std::max(0.f,std::abs(f-cf)-cell*.5f);
      if(dx*dx+dz*dz>corridorCm*corridorCm)
        continue;
      if(NavigationPassage::staticHazard(c)) {
        result.hazard=true;
        return result;
        }
      result.free=result.free || NavigationPassage::seenFree(c);
      }
  return result;
  }
}

bool NavigationPassage::staticHazard(const NavigationMemory::Cell& c) {
  // Only what the walker has seen itself; a familiar tile is a prior, exactly
  // as in the memory teacher. Movable evidence (people, the followed object's
  // own footprints along its trail) does not decide where the carrot goes: the
  // planner keeps its own hard veto for it.
  if(byte(c.familiar)!=0) return false;
  return byte(c.wall)!=0 || byte(c.ledge)!=0 || byte(c.drop)!=0 || byte(c.water)!=0;
  }

bool NavigationPassage::seenFree(const NavigationMemory::Cell& c) {
  return byte(c.familiar)==0 && byte(c.confidence)!=0 && byte(c.free)!=0 && !staticHazard(c);
  }

bool NavigationPassage::near(const NavigationMemory::View& memory, const Vec3& from, const Vec3& to,
                             bool seen, float corridorCm, float footprintCm) {
  const auto& near=memory.near;
  if(!usable(near))
    return !seen;
  const Frame frame(memory.pose);
  if(!frame.valid)
    return !seen;
  const auto a=frame.local(from),b=frame.local(to);
  const float length=std::hypot(b[0]-a[0],b[1]-a[1]);
  const float cell=near.cellCm,half=cell*float(near.size)*.5f;
  const int steps=std::max(1,int(std::ceil(length/(cell*.5f))));
  for(int k=0;k<=steps;++k) {
    const float t=float(k)/float(steps);
    if(length*t<footprintCm)
      continue;
    const float r=a[0]+(b[0]-a[0])*t,f=a[1]+(b[1]-a[1])*t;
    // Outside the near map nothing is seen: neither free ground nor a hazard.
    if(std::abs(r)>half-corridorCm || std::abs(f)>half-corridorCm) {
      if(seen)
        return false;
      continue;
      }
    // A free passage needs seen free ground at every sample; any passage fails
    // on a live hazard anywhere within the corridor.
    const auto sample=nearSample(near,r,f,corridorCm);
    if(sample.hazard || (seen && !sample.free))
      return false;
    }
  return true;
  }

NavigationPassage::Verdict NavigationPassage::check(const NavigationMemory::View& memory, const Vec3& from,
                                                    const Vec3& to, float corridorCm, float footprintCm) {
  const Frame frame(memory.pose);
  const bool hasNear=usable(memory.near),hasFar=usable(memory.far);
  if(!frame.valid || (!hasNear && !hasFar))
    return Verdict::Unknown;
  const auto a=frame.local(from),b=frame.local(to);
  const float length=std::hypot(b[0]-a[0],b[1]-a[1]);
  const float nearHalf=hasNear ? memory.near.cellCm*float(memory.near.size)*.5f-corridorCm : -1.f;
  const float farHalf=hasFar ? memory.far.cellCm*float(memory.far.size)*.5f : -1.f;
  // Half a cell of the layer the sample lies on, as in near().
  const float nearStep=hasNear ? memory.near.cellCm*.5f : memory.far.cellCm*.5f;
  const float farStep=hasFar ? memory.far.cellCm*.5f : nearStep;
  bool free=true;
  float along=0;
  while(true) {
    const float t=length>0 ? std::min(along,length)/length : 0.f;
    const float r=a[0]+(b[0]-a[0])*t,f=a[1]+(b[1]-a[1])*t;
    float step=farStep;
    // The walker's own footprint is not checked, as in near().
    if(along>=footprintCm) {
      if(std::abs(r)<=nearHalf && std::abs(f)<=nearHalf) {
        const auto sample=nearSample(memory.near,r,f,corridorCm);
        if(sample.hazard)
          return Verdict::Closed;
        free=free && sample.free;
        step=nearStep;
        }
      else if(std::abs(r)<farHalf && std::abs(f)<farHalf) {
        const auto size=memory.far.size;
        const auto x=size_t(std::floor(r/memory.far.cellCm+float(size)*.5f));
        const auto z=size_t(std::floor(f/memory.far.cellCm+float(size)*.5f));
        if(x<size && z<size) {
          const auto& c=memory.far.cells[z*size+x];
          if(staticHazard(c) && byte(c.free)==0)
            return Verdict::Closed;
          free=free && seenFree(c);
          }
        else
          free=false;
        }
      else
        free=false;
      }
    else if(std::abs(r)<=nearHalf && std::abs(f)<=nearHalf)
      step=nearStep;
    if(along>=length)
      break;
    along=std::min(along+step,length);
    }
  return free ? Verdict::Free : Verdict::Unknown;
  }
