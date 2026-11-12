#include "lidshade.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

namespace {

constexpr float Rad = float(M_PI/180.0);

void angles(const Vec3& d, float& yaw, float& pitch) {
  const float l = d.length();
  yaw   = std::atan2(d.y,d.z);
  pitch = l>0 ? std::asin(std::clamp(d.x/l,-1.f,1.f)) : 0.f;
  }

// the edge's share at the edge and past it, all of the light `width` away from it, a smooth step between
float ramp(float d, float width, float edge) {
  const float x = std::clamp(d/width,0.f,1.f);
  return edge + (1.f-edge)*x*x*(3.f-2.f*x);
  }

}

std::vector<uint32_t> LidShade::lidTriangles(const std::vector<std::array<uint32_t,3>>& partTriangles,
                                             const std::vector<Vec3>& pos, const Vec3& center, float radius) {
  std::vector<uint32_t> ret;
  const float reach = std::cos(Reach*Rad);
  for(auto& t:partTriangles) {
    bool near = true;
    for(auto v:t) {
      const Vec3  d = pos[v]-center;
      const float l = d.length();
      if(std::abs(l-radius)>0.45f*radius || d.z<reach*l) {
        near = false;
        break;
        }
      }
    if(near)
      ret.insert(ret.end(),t.begin(),t.end());
    }
  return ret;
  }

void LidShade::margins(const std::vector<Vec3>& pos, const std::vector<uint32_t>& tris, const Vec3& center) {
  const float from = From*Rad, step = Step*Rad, reach = Reach*Rad;
  for(auto& s:spans)
    s.clear();
  // each triangle seen from the centre covers an interval of pitch in the columns its yaw spans
  for(size_t i=0; i+2<tris.size(); i+=3) {
    float y[3], p[3];
    for(size_t k=0; k<3; ++k)
      angles(pos[tris[i+k]]-center,y[k],p[k]);
    const float lo = std::min({y[0],y[1],y[2]}), hi = std::max({y[0],y[1],y[2]});
    const int   c0 = std::max(0,int(std::ceil((lo-from)/step)));
    const int   c1 = std::min(Columns-1,int(std::floor((hi-from)/step)));
    for(int c=c0; c<=c1; ++c) {
      const float cy = from+float(c)*step;
      float a = reach, b = -reach;
      for(size_t k=0; k<3; ++k) {
        const size_t n = (k+1)%3;
        if((y[k]-cy)*(y[n]-cy)>0 || y[k]==y[n])
          continue;
        const float q = p[k]+(p[n]-p[k])*(cy-y[k])/(y[n]-y[k]);
        a = std::min(a,q);
        b = std::max(b,q);
        }
      if(a<=b)
        spans[size_t(c)].emplace_back(a,b);
      }
    }

  // the open gaps of a column between the covered intervals: in the middle column (yaw 0) the gap at forward or the
  // nearest to it within 30 degrees, outwards the one that overlaps most the opening of the column before. None: the
  // lids meet there
  const auto pick = [&](int c, bool middle, float wantLo, float wantHi, float& lo, float& hi) {
    auto& s = spans[size_t(c)];
    std::sort(s.begin(),s.end());
    bool  found = false;
    float best  = middle ? 30.f*Rad : 0.f;
    const auto consider = [&](float g0, float g1) {
      if(g1<=g0)
        return;
      if(middle) {
        const float dist = (g0<=0 && g1>=0) ? 0.f : std::min(std::abs(g0),std::abs(g1));
        if(dist>best || (found && dist==best))
          return;
        best = dist;
        } else {
        const float overlap = std::min(g1,wantHi)-std::max(g0,wantLo);
        if(overlap<=best)
          return;
        best = overlap;
        }
      lo    = g0;
      hi    = g1;
      found = true;
      };
    float end = -reach;
    for(auto& [a,b]:s) {
      if(a>end)
        consider(end,std::min(a,reach));
      end = std::max(end,b);
      if(end>=reach)
        break;
      }
    if(end<reach)
      consider(end,reach);
    return found;
    };

  const int mid = int(std::lround(-From/Step));
  float lo = 0, hi = 0;
  if(!pick(mid,true,0,0,lo,hi))
    lo = hi = 0;
  lower[size_t(mid)] = lo;
  upper[size_t(mid)] = hi;
  for(int dir:{1,-1}) {
    float prevLo = lower[size_t(mid)], prevHi = upper[size_t(mid)];
    for(int c=mid+dir; c>=0 && c<Columns; c+=dir) {
      if(!pick(c,false,prevLo,prevHi,lo,hi))
        lo = hi = (prevLo+prevHi)*0.5f;
      lower[size_t(c)] = prevLo = lo;
      upper[size_t(c)] = prevHi = hi;
      }
    }
  valid = true;
  }

float LidShade::light(const Vec3& at, float radius, const Params& p) const {
  if(!valid)
    return 1;
  float yaw, pitch;
  angles(at,yaw,pitch);
  const float t = std::clamp((yaw-From*Rad)/(Step*Rad),0.f,float(Columns-1));
  const int   i = std::min(int(t),Columns-2);
  const float f = t-float(i);
  const float lo = lower[size_t(i)]+(lower[size_t(i+1)]-lower[size_t(i)])*f;
  const float hi = upper[size_t(i)]+(upper[size_t(i+1)]-upper[size_t(i)])*f;
  return ramp((hi-pitch)*radius,p.upperCm,p.upperEdge)*ramp((pitch-lo)*radius,p.lowerCm,p.lowerEdge);
  }
