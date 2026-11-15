#include "grassborder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>

namespace Grass {

namespace {

int32_t units(float cm) {
  return int32_t(std::lround(double(cm)*Unit));
  }

}

bool triLess(const Tri& l, const Tri& r) {
  return std::tie(l.x[0],l.z[0],l.y[0],l.x[1],l.z[1],l.y[1],l.x[2],l.z[2],l.y[2],l.slot) <
         std::tie(r.x[0],r.z[0],r.y[0],r.x[1],r.z[1],r.y[1],r.x[2],r.z[2],r.y[2],r.slot);
  }

bool insideXZ(const int32_t x[3], const int32_t z[3], int32_t px, int32_t pz) {
  int sign = 0;
  for(int i=0; i<3; ++i) {
    const int a = i, b = (i+1)%3;
    const int64_t w = int64_t(x[b]-x[a])*int64_t(pz-z[a]) - int64_t(z[b]-z[a])*int64_t(px-x[a]);
    if(w==0)
      continue;
    const int s = w>0 ? 1 : -1;
    if(sign!=0 && s!=sign)
      return false;
    sign = s;
    }
  return true;
  }

void addVerge(Summary& out, const float pos[3][3], const float normal[3][3], uint16_t slot, float reach, float margin) {
  if(normal[0][1]+normal[1][1]+normal[2][1]<=0.6f)
    return;
  VergeTri t;
  t.slot   = slot;
  t.reach  = uint8_t(std::clamp(std::lround(reach),0l,255l));
  t.margin = uint8_t(std::clamp(std::lround(margin),0l,255l));
  for(int i=0; i<3; ++i) {
    t.x[i] = units(pos[i][0]);
    t.y[i] = units(pos[i][1]);
    t.z[i] = units(pos[i][2]);
    }
  const int32_t ext = std::max({std::abs(t.x[1]-t.x[0]), std::abs(t.x[2]-t.x[0]), std::abs(t.x[2]-t.x[1]),
                                std::abs(t.z[1]-t.z[0]), std::abs(t.z[2]-t.z[0]), std::abs(t.z[2]-t.z[1])});
  if(ext>=MaxExtent)
    return;
  out.verge.push_back(t);
  }

void openEdges(std::span<const Tri*const> tris, std::span<const VergeTri*const> verge,
               const int32_t lo[2], const int32_t hi[2], int32_t band, std::vector<OpenEdge>& out) {
  struct Box final {
    int32_t minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    };
  std::vector<Box> boxes(tris.size());
  for(size_t i=0; i<tris.size(); ++i) {
    const auto& t = *tris[i];
    boxes[i] = Box{std::min({t.x[0],t.x[1],t.x[2]}), std::max({t.x[0],t.x[1],t.x[2]}),
                   std::min({t.z[0],t.z[1],t.z[2]}), std::max({t.z[0],t.z[1],t.z[2]})};
    }
  auto onGrass = [&](int32_t px, int32_t pz) {
    for(size_t i=0; i<tris.size(); ++i) {
      const Box& r = boxes[i];
      if(px<r.minX || px>r.maxX || pz<r.minZ || pz>r.maxZ)
        continue;
      if(insideXZ(tris[i]->x,tris[i]->z,px,pz))
        return true;
      }
    return false;
    };
  // the verge of the grass `slot` under the point, at about the height y
  auto onVerge = [&](int32_t px, int32_t pz, int32_t y, uint16_t slot) -> const VergeTri* {
    for(const VergeTri* g:verge) {
      if(g->slot!=slot ||
         px<std::min({g->x[0],g->x[1],g->x[2]}) || px>std::max({g->x[0],g->x[1],g->x[2]}) ||
         pz<std::min({g->z[0],g->z[1],g->z[2]}) || pz>std::max({g->z[0],g->z[1],g->z[2]}) || !insideXZ(g->x,g->z,px,pz))
        continue;
      const int64_t area2 = int64_t(g->x[1]-g->x[0])*int64_t(g->z[2]-g->z[0]) - int64_t(g->x[2]-g->x[0])*int64_t(g->z[1]-g->z[0]);
      if(area2==0)
        continue;
      int64_t sum = 0;
      for(int i=0; i<3; ++i) {
        const int a = (i+1)%3, b = (i+2)%3;
        sum += (int64_t(g->x[b]-g->x[a])*int64_t(pz-g->z[a]) - int64_t(g->z[b]-g->z[a])*int64_t(px-g->x[a]))*int64_t(g->y[i]);
        }
      if(std::abs(int32_t(sum/area2)-y)<=VergeStep)
        return g;
      }
    return nullptr;
    };

  struct Edge final {
    int32_t a[3], b[3]; // sorted ends
    int32_t from[3], to[3]; // as the triangle goes round: x, z, y
    uint32_t tri;
    };
  std::vector<Edge> edges;
  edges.reserve(tris.size()*3);
  for(size_t id=0; id<tris.size(); ++id) {
    const auto& t = *tris[id];
    for(int i=0; i<3; ++i) {
      const int j = (i+1)%3;
      Edge e;
      e.tri = uint32_t(id);
      e.from[0] = t.x[i]; e.from[1] = t.z[i]; e.from[2] = t.y[i];
      e.to[0]   = t.x[j]; e.to[1]   = t.z[j]; e.to[2]   = t.y[j];
      const std::array<int32_t,3> p = {t.x[i],t.z[i],t.y[i]}, q = {t.x[j],t.z[j],t.y[j]};
      const auto& first = p<q ? p : q; const auto& second = p<q ? q : p;
      std::copy(first.begin(),first.end(),e.a);
      std::copy(second.begin(),second.end(),e.b);
      edges.push_back(e);
      }
    }
  auto key = [](const Edge& e) { return std::tie(e.a[0],e.a[1],e.a[2],e.b[0],e.b[1],e.b[2]); };
  std::sort(edges.begin(),edges.end(),[&](const Edge& l, const Edge& r) {
    return std::tie(l.a[0],l.a[1],l.a[2],l.b[0],l.b[1],l.b[2],l.from[0],l.from[1]) <
           std::tie(r.a[0],r.a[1],r.a[2],r.b[0],r.b[1],r.b[2],r.from[0],r.from[1]);
    });
  for(size_t i=0; i<edges.size();) {
    size_t n = i+1;
    while(n<edges.size() && key(edges[n])==key(edges[i]))
      ++n;
    const Edge& e = edges[i];
    const bool  single = n==i+1;
    i = n;
    if(!single)
      continue;
    if(std::max(e.from[0],e.to[0])<lo[0]-band || std::min(e.from[0],e.to[0])>hi[0]+band ||
       std::max(e.from[1],e.to[1])<lo[1]-band || std::min(e.from[1],e.to[1])>hi[1]+band)
      continue;
    const double dx = double(e.to[0]-e.from[0]), dz = double(e.to[1]-e.from[1]);
    const double len = std::sqrt(dx*dx+dz*dz);
    if(len<1.0)
      continue;
    // the triangle is counter-clockwise: its inside is on the left of the edge, (-dz,dx)
    const double    ox = dz/len, oz = -dx/len;
    const int       pieces = std::max(1,int(std::ceil(len/(50.0*Unit))));
    int             start  = -1;
    const VergeTri* along  = nullptr;
    for(int k=0; k<=pieces; ++k) {
      bool            open = false;
      const VergeTri* road = nullptr;
      if(k<pieces) {
        const double m = (double(k)+0.5)/double(pieces);
        const auto px = int32_t(std::lround(double(e.from[0]) + dx*m + ox*6.0*Unit));
        const auto pz = int32_t(std::lround(double(e.from[1]) + dz*m + oz*6.0*Unit));
        open = !onGrass(px,pz);
        if(open && !verge.empty())
          road = onVerge(px,pz,int32_t(std::lround(double(e.from[2]) + double(e.to[2]-e.from[2])*m)),tris[e.tri]->slot);
        }
      const bool same = (road==nullptr)==(along==nullptr) &&
                        (road==nullptr || (road->reach==along->reach && road->margin==along->margin));
      if(start>=0 && (!open || !same)) {
        OpenEdge s;
        s.ax = int32_t(std::lround((double(e.from[0]) + dx*double(start)/double(pieces))/Unit));
        s.az = int32_t(std::lround((double(e.from[1]) + dz*double(start)/double(pieces))/Unit));
        s.bx = int32_t(std::lround((double(e.from[0]) + dx*double(k)/double(pieces))/Unit));
        s.bz = int32_t(std::lround((double(e.from[1]) + dz*double(k)/double(pieces))/Unit));
        s.outX = float(ox);
        s.outZ = float(oz);
        s.tri  = e.tri;
        if(along!=nullptr) {
          s.verge  = true;
          s.reach  = along->reach;
          s.margin = along->margin;
          }
        out.push_back(s);
        start = -1;
        }
      if(open && start<0) {
        start = k;
        along = road;
        }
      }
    }
  }

std::vector<BorderSeg> chunkBorder(const Summary& own, std::span<const Summary*const> around) {
  std::vector<BorderSeg> ret;
  if(own.grass.empty())
    return ret;
  bool any = false;
  for(const Summary* s:around)
    any = any || (s!=nullptr && !s->verge.empty());
  if(!any)
    return ret;

  int32_t lo[2] = {INT32_MAX,INT32_MAX}, hi[2] = {INT32_MIN,INT32_MIN};
  for(const auto& t:own.grass)
    for(int i=0; i<3; ++i) {
      lo[0] = std::min(lo[0],t.x[i]); hi[0] = std::max(hi[0],t.x[i]);
      lo[1] = std::min(lo[1],t.z[i]); hi[1] = std::max(hi[1],t.z[i]);
      }
  const int32_t pad = 100*Unit;
  std::vector<const Tri*>      tris;
  std::vector<const VergeTri*> verge;
  for(const Summary* s:around) {
    if(s==nullptr)
      continue;
    for(const auto& t:s->grass) {
      if(std::max({t.x[0],t.x[1],t.x[2]})<lo[0]-pad || std::min({t.x[0],t.x[1],t.x[2]})>hi[0]+pad ||
         std::max({t.z[0],t.z[1],t.z[2]})<lo[1]-pad || std::min({t.z[0],t.z[1],t.z[2]})>hi[1]+pad)
        continue;
      tris.push_back(&t);
      }
    for(const auto& t:s->verge) {
      if(std::max({t.x[0],t.x[1],t.x[2]})<lo[0]-pad || std::min({t.x[0],t.x[1],t.x[2]})>hi[0]+pad ||
         std::max({t.z[0],t.z[1],t.z[2]})<lo[1]-pad || std::min({t.z[0],t.z[1],t.z[2]})>hi[1]+pad)
        continue;
      verge.push_back(&t);
      }
    }
  if(verge.empty())
    return ret;
  std::sort(tris.begin(),tris.end(),[](const Tri* l, const Tri* r) { return triLess(*l,*r); });

  std::vector<OpenEdge> edges;
  openEdges(tris,verge,lo,hi,0,edges);
  const Tri* first = own.grass.data();
  const Tri* last  = first + own.grass.size();
  for(const auto& e:edges) {
    const Tri* t = tris[e.tri];
    if(!e.verge || t<first || t>=last)
      continue;
    ret.push_back(BorderSeg{float(e.ax),float(e.az),float(e.bx),float(e.bz),e.reach,e.margin});
    }
  return ret;
  }

std::vector<uint32_t> packBorder(std::span<const BorderSeg> segs) {
  constexpr size_t Head = 4, Table = size_t(BorderSide)*BorderSide, Words = 5;
  std::vector<uint32_t> ret(Head+Table, 0u);
  ret[0] = segs.empty() ? 0u : BorderSide;
  ret[1] = uint32_t(BorderBin);
  if(segs.empty()) {
    ret.resize(Head);
    return ret;
    }
  auto forBins = [&](const BorderSeg& s, auto&& fn) {
    const auto x0 = int32_t(std::floor((std::min(s.ax,s.bx)-BorderPad)/BorderBin)), x1 = int32_t(std::floor((std::max(s.ax,s.bx)+BorderPad)/BorderBin));
    const auto z0 = int32_t(std::floor((std::min(s.az,s.bz)-BorderPad)/BorderBin)), z1 = int32_t(std::floor((std::max(s.az,s.bz)+BorderPad)/BorderBin));
    if(x1-x0>=int32_t(BorderSide) || z1-z0>=int32_t(BorderSide))
      return;
    for(int32_t z=z0; z<=z1; ++z)
      for(int32_t x=x0; x<=x1; ++x)
        fn(size_t(uint32_t(z) & (BorderSide-1))*BorderSide + size_t(uint32_t(x) & (BorderSide-1)));
    };
  std::vector<uint8_t> count(Table, 0);
  for(const auto& s:segs)
    forBins(s,[&](size_t b) { if(count[b]<63) ++count[b]; });
  size_t at = Head+Table;
  for(size_t b=0; b<Table; ++b) {
    ret[Head+b] = uint32_t(at << 6);
    at += size_t(count[b])*Words;
    }
  ret.resize(at);
  for(const auto& s:segs)
    forBins(s,[&](size_t b) {
      uint32_t& e = ret[Head+b];
      if((e & 63u)>=63u)
        return;
      const size_t   to = size_t(e >> 6) + size_t(e & 63u)*Words;
      const float    f[4] = {s.ax,s.az,s.bx,s.bz};
      std::memcpy(&ret[to],f,sizeof(f));
      ret[to+4] = (s.reach & 0xFFu) | ((s.margin & 0xFFu) << 8);
      ++e;
      });
  return ret;
  }

BorderHit nearestBorder(std::span<const uint32_t> grid, float x, float z, float limit) {
  BorderHit hit;
  if(grid.size()<4 || grid[0]==0)
    return hit;
  const uint32_t side = grid[0];
  const float    bin  = float(grid[1]);
  const uint32_t ix = uint32_t(int32_t(std::floor(x/bin))) & (side-1), iz = uint32_t(int32_t(std::floor(z/bin))) & (side-1);
  const uint32_t e  = grid[4 + iz*side + ix];
  float best = limit;
  for(uint32_t i=0; i<(e & 63u); ++i) {
    const size_t at = size_t(e >> 6) + size_t(i)*5;
    float f[4];
    std::memcpy(f,&grid[at],sizeof(f));
    const float abx = f[2]-f[0], abz = f[3]-f[1], apx = x-f[0], apz = z-f[1];
    const float t   = std::clamp((apx*abx + apz*abz)/std::max(abx*abx + abz*abz,1e-6f), 0.f, 1.f);
    const float dx  = apx-abx*t, dz = apz-abz*t;
    const float d   = std::sqrt(dx*dx + dz*dz);
    if(d<best) {
      best       = d;
      hit.found  = true;
      hit.reach  = float(grid[at+4] & 0xFFu);
      hit.margin = float((grid[at+4] >> 8) & 0xFFu);
      }
    }
  hit.dist = best;
  return hit;
  }

}
