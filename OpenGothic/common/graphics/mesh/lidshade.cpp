#include "lidshade.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

namespace {

constexpr float Rad = float(M_PI/180.0);

// the columns as tangents of their yaw: a direction (x, y, z) with z > 0 is in column yaw where y/z = tan(yaw)
const std::array<float,LidShade::Columns>& columnTan() {
  static const auto ret = [] {
    std::array<float,LidShade::Columns> t{};
    for(int c=0; c<LidShade::Columns; ++c)
      t[size_t(c)] = std::tan((LidShade::From+float(c)*LidShade::Step)*Rad);
    return t;
    }();
  return ret;
  }

// the edge's share at the edge and past it, all of the light `width` away from it, a smooth step between
float ramp(float d, float width, float edge) {
  const float x = std::clamp(d/width,0.f,1.f);
  return edge + (1.f-edge)*x*x*(3.f-2.f*x);
  }

}

LidShade::Lids LidShade::lids(const std::vector<std::array<uint32_t,3>>& partTriangles, const std::vector<Vec3>& pos,
                              const Vec3& center, float radius) {
  Lids ret;
  const float reach = std::cos(Reach*Rad);
  std::vector<uint32_t> index;
  for(auto& t:partTriangles) {
    bool near = true;
    for(auto v:t) {
      const Vec3  d = pos[v]-center;
      const float l = d.length();
      if(std::abs(l-radius)>Band*radius || d.z<reach*l) {
        near = false;
        break;
        }
      }
    if(!near)
      continue;
    for(auto v:t) {
      if(index.size()<=v)
        index.resize(v+1,uint32_t(-1));
      if(index[v]==uint32_t(-1)) {
        index[v] = uint32_t(ret.verts.size());
        ret.verts.push_back(v);
        }
      ret.corners.push_back(index[v]);
      }
    }
  return ret;
  }

bool LidShade::margins(const std::vector<Vec3>& pos, const Lids& lids, const Vec3& center) {
  if(valid && seen.size()==lids.verts.size()) {
    bool still = true;
    for(size_t i=0; i<seen.size() && still; ++i)
      still = (pos[lids.verts[i]]-seen[i]).quadLength()<StillCm*StillCm;
    if(still)
      return false;
    }
  seen.resize(lids.verts.size());
  for(size_t i=0; i<seen.size(); ++i)
    seen[i] = pos[lids.verts[i]];
  // Seen from the centre a direction is (tan yaw, tan pitch) = (y/z, x/sqrt(y^2+z^2)): no trigonometry per vertex,
  // a column is a line of tan yaw, and the order along it is that of pitch
  const auto& col   = columnTan();
  const float reach = std::tan(Reach*Rad);
  across.resize(lids.verts.size());
  rise  .resize(lids.verts.size());
  for(size_t i=0; i<lids.verts.size(); ++i) {
    const Vec3  d = pos[lids.verts[i]]-center;
    const float h = std::sqrt(d.y*d.y+d.z*d.z);
    const float z = std::max(d.z,1e-3f*h+1e-6f);  // a lid's corner swung behind the centre: at the side
    across[i] = d.y/z;
    rise[i]   = std::clamp(d.x/std::max(h,1e-6f),-reach,reach);
    }
  for(auto& s:spans)
    s.clear();
  // each triangle covers an interval of pitch in the columns its yaw spans
  for(size_t i=0; i+2<lids.corners.size(); i+=3) {
    float y[3], p[3];
    for(size_t k=0; k<3; ++k) {
      y[k] = across[lids.corners[i+k]];
      p[k] = rise  [lids.corners[i+k]];
      }
    const float lo = std::min({y[0],y[1],y[2]}), hi = std::max({y[0],y[1],y[2]});
    const auto  c0 = std::lower_bound(col.begin(),col.end(),lo)-col.begin();
    for(auto c=c0; c<Columns && col[size_t(c)]<=hi; ++c) {
      const float cy = col[size_t(c)];
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

  // the open gaps of the middle column (yaw 0) between the covered intervals: the gap at forward or the nearest to
  // it within 30 degrees. None: the lids meet there
  const float near30 = std::tan(30.f*Rad);
  const auto pick = [&](int c, float& lo, float& hi) {
    auto& s = spans[size_t(c)];
    std::sort(s.begin(),s.end());
    bool  found = false;
    float best  = near30;
    const auto consider = [&](float g0, float g1) {
      if(g1<=g0)
        return;
      const float dist = (g0<=0 && g1>=0) ? 0.f : std::min(std::abs(g0),std::abs(g1));
      if(dist>best || (found && dist==best))
        return;
      best  = dist;
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
  if(!pick(mid,lo,hi))
    lo = hi = 0;
  lower[size_t(mid)] = lo;
  upper[size_t(mid)] = hi;
  // outwards: the gap round the middle of the opening of the column before, by one pass over the column's intervals;
  // an interval over that point closes the lids there, and so on to the corner
  for(int dir:{1,-1}) {
    float prevLo = lower[size_t(mid)], prevHi = upper[size_t(mid)];
    for(int c=mid+dir; c>=0 && c<Columns; c+=dir) {
      const float at = (prevLo+prevHi)*0.5f;
      lo = -reach;
      hi = reach;
      bool shut = prevLo>=prevHi;
      for(auto& [a,b]:spans[size_t(c)]) {
        if(a<at && b>at)
          shut = true;
        if(b<=at)
          lo = std::max(lo,b);
        if(a>=at)
          hi = std::min(hi,a);
        }
      if(shut)
        lo = hi = at;
      lower[size_t(c)] = prevLo = lo;
      upper[size_t(c)] = prevHi = hi;
      }
    }
  // tangents to radians of pitch
  for(int c=0; c<Columns; ++c) {
    lower[size_t(c)] = std::atan(lower[size_t(c)]);
    upper[size_t(c)] = std::atan(upper[size_t(c)]);
    }
  valid = true;
  return true;
  }

float LidShade::light(const Vec3& at, float radius, const Params& p) const {
  if(!valid)
    return 1;
  // the back of the ball is under the lids whatever they do: the share at their edges
  if(at.z<-0.3f*radius)
    return p.upperEdge*p.lowerEdge;
  const float h     = std::sqrt(at.y*at.y+at.z*at.z);
  const float yaw   = std::atan2(at.y,at.z);
  const float pitch = std::atan2(at.x,h);
  const float t = std::clamp((yaw-From*Rad)/(Step*Rad),0.f,float(Columns-1));
  const int   i = std::min(int(t),Columns-2);
  const float f = t-float(i);
  const float lo = lower[size_t(i)]+(lower[size_t(i+1)]-lower[size_t(i)])*f;
  const float hi = upper[size_t(i)]+(upper[size_t(i+1)]-upper[size_t(i)])*f;
  return ramp((hi-pitch)*radius,p.upperCm,p.upperEdge)*ramp((pitch-lo)*radius,p.lowerCm,p.lowerEdge);
  }
