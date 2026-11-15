#include "grassplace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include "graphics/grass/grassborder.h"
#include "graphics/grass/grasscover.h"

namespace Grass {

namespace {

// A triangle made ready: its edges' functions, its plane
struct Ready final {
  const Tri* t = nullptr;
  int64_t    area2 = 0;
  int32_t    minX = 0, maxX = 0, minZ = 0, maxZ = 0;
  int32_t    tanQ12 = 0; // of its slope
  };

// An edge on a point's tie goes to one of the two triangles that share it
bool edgeOwns(int32_t dx, int32_t dz) {
  return dz>0 || (dz==0 && dx<0);
  }

// w: the three edge functions of the point; inside by the rule of the tie
bool inside(const Tri& t, int32_t px, int32_t pz, int64_t w[3]) {
  for(int i=0; i<3; ++i) {
    const int a = (i+1)%3, b = (i+2)%3;
    const int32_t dx = t.x[b]-t.x[a], dz = t.z[b]-t.z[a];
    w[i] = int64_t(dx)*int64_t(pz-t.z[a]) - int64_t(dz)*int64_t(px-t.x[a]);
    if(w[i]<0 || (w[i]==0 && !edgeOwns(dx,dz)))
      return false;
    }
  return true;
  }

int32_t units(float cm) {
  return int32_t(std::lround(double(cm)*Unit));
  }

using Segment = OpenEdge;

struct Raw final { // a blade before it is packed
  int32_t  x = 0, y = 0, z = 0;
  uint16_t slot = 0;
  uint8_t  kind = 0;
  uint8_t  rank = 0, height = 0, facing = 0, phase = 0, lean = 0, width = 0, tint = 0, nx = 0, nz = 0;
  uint16_t u = 0, v = 0;
  };

}

void addGrass(Summary& out, const float pos[3][3], const float uv[3][2], const float normal[3][3], uint16_t slot) {
  if(normal[0][1]+normal[1][1]+normal[2][1]<=0.6f)
    return; // faces down or stands upright
  Tri t;
  t.slot = slot;
  for(int i=0; i<3; ++i) {
    t.x[i]  = units(pos[i][0]);
    t.y[i]  = units(pos[i][1]);
    t.z[i]  = units(pos[i][2]);
    t.u[i]  = uv[i][0];
    t.v[i]  = uv[i][1];
    t.nx[i] = normal[i][0];
    t.ny[i] = normal[i][1];
    t.nz[i] = normal[i][2];
    }
  const int32_t ext = std::max({std::abs(t.x[1]-t.x[0]), std::abs(t.x[2]-t.x[0]), std::abs(t.x[2]-t.x[1]),
                                std::abs(t.y[1]-t.y[0]), std::abs(t.y[2]-t.y[0]), std::abs(t.y[2]-t.y[1]),
                                std::abs(t.z[1]-t.z[0]), std::abs(t.z[2]-t.z[0]), std::abs(t.z[2]-t.z[1])});
  if(ext>=MaxExtent)
    return;
  const int64_t area2 = int64_t(t.x[1]-t.x[0])*int64_t(t.z[2]-t.z[0]) - int64_t(t.x[2]-t.x[0])*int64_t(t.z[1]-t.z[0]);
  if(area2==0)
    return;
  if(area2<0) {
    std::swap(t.x[1],t.x[2]);   std::swap(t.y[1],t.y[2]);   std::swap(t.z[1],t.z[2]);
    std::swap(t.u[1],t.u[2]);   std::swap(t.v[1],t.v[2]);
    std::swap(t.nx[1],t.nx[2]); std::swap(t.ny[1],t.ny[2]); std::swap(t.nz[1],t.nz[2]);
    }
  out.grass.push_back(t);
  }

void addWater(Summary& out, const float pos[3][3]) {
  WaterTri t;
  for(int i=0; i<3; ++i) {
    t.x[i] = units(pos[i][0]);
    t.y[i] = units(pos[i][1]);
    t.z[i] = units(pos[i][2]);
    }
  out.water.push_back(t);
  }

void addBeside(Summary& out, const float pos[3][3], const float normal[3][3]) {
  if(normal[0][1]+normal[1][1]+normal[2][1]<=0.6f)
    return;
  WaterTri t;
  for(int i=0; i<3; ++i) {
    t.x[i] = units(pos[i][0]);
    t.y[i] = units(pos[i][1]);
    t.z[i] = units(pos[i][2]);
    }
  out.beside.push_back(t);
  }

BladeInfo decode(const Blade& b, int32_t cx, int32_t cz, int32_t minY) {
  BladeInfo r;
  r.x      = cx*CellSize - CellMargin + int32_t(b.w[0] & 0x3FFFu);
  r.z      = cz*CellSize - CellMargin + int32_t((b.w[0] >> 14) & 0x3FFFu);
  r.y      = minY + int32_t(b.w[1] & 0xFFFFu)*2;
  r.height = float((b.w[1] >> 16) & 0xFFu)*0.25f;
  r.rank   = uint8_t(b.w[1] >> 24);
  return r;
  }

CellBlades buildCell(const CellInput& in) {
  CellBlades ret;
  if(in.params.empty() || in.rankLimit==0)
    return ret;

  const int32_t x0 = in.cx*CellSize, z0 = in.cz*CellSize;
  const int32_t lo[2] = {x0-CellMargin, z0-CellMargin};
  const int32_t hi[2] = {x0+CellSize+CellMargin, z0+CellSize+CellMargin};

  // per slot, in whole numbers
  struct Slot final {
    int32_t densityQ8 = 0, tanMax = 0, tanFull = 0, edgeCm = 0;
    };
  std::vector<Slot> slots(in.params.size());
  int32_t maxEdge = 0, maxDensity = 0;
  bool    anyFine = false, anyBroad = false;
  for(size_t i=0; i<slots.size(); ++i) {
    const auto& p = in.params[i];
    if(!p.on)
      continue;
    slots[i].densityQ8 = int32_t(std::lround(double(p.density)*double(in.density)*256.0));
    slots[i].tanMax    = int32_t(std::lround(std::tan(double(p.slope)*M_PI/180.0)*4096.0));
    slots[i].tanFull   = int32_t(std::lround(std::tan(double(std::max(p.slope-10.f,0.f))*M_PI/180.0)*4096.0));
    slots[i].edgeCm    = int32_t(std::lround(p.edge));
    anyFine            = anyFine || (p.cover && p.fine);
    anyBroad           = anyBroad || (p.cover && p.fine && p.broad>0.f);
    maxEdge    = std::max(maxEdge,slots[i].edgeCm);
    maxDensity = std::max(maxDensity,slots[i].densityQ8);
    }
  if(maxDensity<=0)
    return ret;
  // the ground with `verge` round the cell: the sward of a slot with the cover comes onto it
  constexpr int32_t VergeBand = (MaxVergeCm+5)*Unit;
  std::vector<const VergeTri*> verge;
  for(const Summary* s:in.summaries) {
    if(s==nullptr)
      continue;
    for(const auto& t:s->verge) {
      if(size_t(t.slot)>=slots.size() || !in.params[t.slot].cover ||
         std::max({t.x[0],t.x[1],t.x[2]})<lo[0]-VergeBand || std::min({t.x[0],t.x[1],t.x[2]})>hi[0]+VergeBand ||
         std::max({t.z[0],t.z[1],t.z[2]})<lo[1]-VergeBand || std::min({t.z[0],t.z[1],t.z[2]})>hi[1]+VergeBand)
        continue;
      verge.push_back(&t);
      }
    }
  // an open edge is felt to twice the record's band (the noise on the distance), a verge's to the tongues' end
  // and by a sward's broad blades as far as they keep clear of it
  const int32_t band  = std::max({2*maxEdge*Unit, verge.empty() ? 0 : VergeBand,
                                  (anyBroad && maxEdge>0) ? CoverBroadTo*Unit : 0});
  const int32_t reach = CellMargin + band + 100*Unit;

  // the triangles round the cell, in an order that does not depend on the chunks'
  std::vector<Ready> tris;
  for(const Summary* s:in.summaries) {
    if(s==nullptr)
      continue;
    for(const auto& t:s->grass) {
      if(size_t(t.slot)>=slots.size() || slots[t.slot].densityQ8<=0)
        continue;
      Ready r;
      r.t    = &t;
      r.minX = std::min({t.x[0],t.x[1],t.x[2]}); r.maxX = std::max({t.x[0],t.x[1],t.x[2]});
      r.minZ = std::min({t.z[0],t.z[1],t.z[2]}); r.maxZ = std::max({t.z[0],t.z[1],t.z[2]});
      if(r.maxX<lo[0]-reach || r.minX>hi[0]+reach || r.maxZ<lo[1]-reach || r.minZ>hi[1]+reach)
        continue;
      r.area2 = int64_t(t.x[1]-t.x[0])*int64_t(t.z[2]-t.z[0]) - int64_t(t.x[2]-t.x[0])*int64_t(t.z[1]-t.z[0]);
      // the slope: the plane's normal brought under 2^15, its tangent in 1/4096
      int64_t n[3] = {
        int64_t(t.y[1]-t.y[0])*int64_t(t.z[2]-t.z[0]) - int64_t(t.z[1]-t.z[0])*int64_t(t.y[2]-t.y[0]),
        r.area2,
        int64_t(t.x[1]-t.x[0])*int64_t(t.y[2]-t.y[0]) - int64_t(t.y[1]-t.y[0])*int64_t(t.x[2]-t.x[0])};
      while(std::max({std::abs(n[0]),std::abs(n[1]),std::abs(n[2])})>=(int64_t(1)<<15)) {
        n[0] /= 2; n[1] /= 2; n[2] /= 2;
        }
      if(n[1]==0)
        continue;
      r.tanQ12 = int32_t(std::min<uint64_t>(isqrt((uint64_t(n[0]*n[0]+n[2]*n[2]) << 24)/uint64_t(n[1]*n[1])), 1u<<20));
      tris.push_back(r);
      }
    }
  if(tris.empty())
    return ret;
  std::sort(tris.begin(),tris.end(),[](const Ready& a, const Ready& b) { return triLess(*a.t,*b.t); });

  // bins over the cell and its margin: the triangles and the open edges near each
  constexpr int32_t Bins = 16;
  const int32_t binSize = (hi[0]-lo[0]+Bins-1)/Bins;
  std::array<std::vector<uint32_t>,Bins*Bins> triBins, segBins, vergeBins;
  auto forBins = [&](int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ, auto&& fn) {
    const int32_t bx0 = std::max((minX-lo[0])/binSize,0), bx1 = std::min((maxX-lo[0])/binSize,Bins-1);
    const int32_t bz0 = std::max((minZ-lo[1])/binSize,0), bz1 = std::min((maxZ-lo[1])/binSize,Bins-1);
    if(maxX<lo[0] || maxZ<lo[1])
      return;
    for(int32_t bz=bz0; bz<=bz1; ++bz)
      for(int32_t bx=bx0; bx<=bx1; ++bx)
        fn(size_t(bz*Bins+bx));
    };
  for(size_t i=0; i<tris.size(); ++i)
    forBins(tris[i].minX,tris[i].maxX,tris[i].minZ,tris[i].maxZ,[&](size_t b) { triBins[b].push_back(uint32_t(i)); });

  // The highest triangle the point lies in. w: its edge functions there
  auto locate = [&](int32_t px, int32_t pz, int64_t w[3], int32_t& y) -> const Ready* {
    if(px<lo[0] || px>=hi[0] || pz<lo[1] || pz>=hi[1])
      return nullptr;
    const Ready* best = nullptr;
    for(uint32_t id:triBins[size_t(((pz-lo[1])/binSize)*Bins + (px-lo[0])/binSize)]) {
      const Ready& r = tris[id];
      if(px<r.minX || px>r.maxX || pz<r.minZ || pz>r.maxZ)
        continue;
      int64_t e[3];
      if(!inside(*r.t,px,pz,e))
        continue;
      const auto& t = *r.t;
      const auto  h = int32_t((e[0]*t.y[0] + e[1]*t.y[1] + e[2]*t.y[2])/r.area2);
      if(best==nullptr || h>y) {
        best = &r;
        y    = h;
        w[0] = e[0]; w[1] = e[1]; w[2] = e[2];
        }
      }
    return best;
    };

  // Open edges (grassborder.h); those along a verge apart as well
  std::vector<Segment> segments;
  bool                 anyVerge = false;
  if(maxEdge>0 || !verge.empty()) {
    std::vector<const Tri*> list(tris.size());
    for(size_t i=0; i<tris.size(); ++i)
      list[i] = tris[i].t;
    openEdges(list,verge,lo,hi,band,segments);
    for(size_t i=0; i<segments.size(); ++i) {
      const auto& s = segments[i];
      forBins(std::min(s.ax,s.bx)*Unit-band, std::max(s.ax,s.bx)*Unit+band,
              std::min(s.az,s.bz)*Unit-band, std::max(s.az,s.bz)*Unit+band,
              [&](size_t b) { segBins[b].push_back(uint32_t(i)); });
      if(!s.verge)
        continue;
      anyVerge = true;
      forBins(std::min(s.ax,s.bx)*Unit-VergeBand, std::max(s.ax,s.bx)*Unit+VergeBand,
              std::min(s.az,s.bz)*Unit-VergeBand, std::max(s.az,s.bz)*Unit+VergeBand,
              [&](size_t b) { vergeBins[b].push_back(uint32_t(i)); });
      }
    }
  // cm to the nearest open edge, `limit` when none is nearer; which: that edge
  auto edgeDistance = [&](int32_t px, int32_t pz, int32_t limit, const Segment*& which) {
    const auto& list = segBins[size_t(((pz-lo[1])/binSize)*Bins + (px-lo[0])/binSize)];
    const int32_t x = floorDiv(px,Unit), z = floorDiv(pz,Unit);
    int64_t best = int64_t(limit)*limit;
    for(uint32_t id:list) {
      const Segment& s = segments[id];
      const int64_t abx = s.bx-s.ax, abz = s.bz-s.az, apx = x-s.ax, apz = z-s.az;
      const int64_t len2 = abx*abx + abz*abz, t = apx*abx + apz*abz;
      int64_t d2;
      if(t<=0 || len2==0) {
        d2 = apx*apx + apz*apz;
        } else if(t>=len2) {
        d2 = (x-s.bx)*int64_t(x-s.bx) + (z-s.bz)*int64_t(z-s.bz);
        } else {
        const int64_t cross = apx*abz - apz*abx;
        d2 = cross*cross/len2;
        }
      if(d2<best) {
        best  = d2;
        which = &s;
        }
      }
    return int32_t(isqrt(uint64_t(best)));
    };

  // units to the nearest piece of a border with a verge, `limit` when none is nearer; which: that piece
  auto vergeDistance = [&](int32_t px, int32_t pz, int32_t limit, const Segment*& which) {
    int64_t best = limit;
    if(px<lo[0] || px>=hi[0] || pz<lo[1] || pz>=hi[1])
      return int32_t(best);
    for(uint32_t id:vergeBins[size_t(((pz-lo[1])/binSize)*Bins + (px-lo[0])/binSize)]) {
      const Segment& s = segments[id];
      const int64_t ax = int64_t(s.ax)*Unit, az = int64_t(s.az)*Unit;
      const int64_t abx = int64_t(s.bx)*Unit-ax, abz = int64_t(s.bz)*Unit-az, apx = px-ax, apz = pz-az;
      const int64_t len2 = abx*abx + abz*abz, t = apx*abx + apz*abz;
      int64_t d;
      if(t<=0 || len2==0) {
        d = isqrt(uint64_t(apx*apx + apz*apz));
        } else if(t>=len2) {
        d = isqrt(uint64_t((apx-abx)*(apx-abx) + (apz-abz)*(apz-abz)));
        } else {
        d = std::abs(apx*abz - apz*abx)/int64_t(isqrt(uint64_t(len2)));
        }
      if(d<best) {
        best  = d;
        which = &s;
        }
      }
    return int32_t(best);
    };

  // water over the cell
  std::vector<const WaterTri*> water;
  for(const Summary* s:in.summaries) {
    if(s==nullptr)
      continue;
    for(const auto& t:s->water) {
      if(std::max({t.x[0],t.x[1],t.x[2]})<lo[0] || std::min({t.x[0],t.x[1],t.x[2]})>hi[0] ||
         std::max({t.z[0],t.z[1],t.z[2]})<lo[1] || std::min({t.z[0],t.z[1],t.z[2]})>hi[1])
        continue;
      water.push_back(&t);
      }
    }
  auto underWater = [&](int32_t px, int32_t pz, int32_t y) {
    for(const WaterTri* t:water) {
      int32_t x[3] = {t->x[0],t->x[1],t->x[2]}, z[3] = {t->z[0],t->z[1],t->z[2]};
      if(std::max({t->y[0],t->y[1],t->y[2]})>y && insideXZ(x,z,px,pz))
        return true;
      }
    return false;
    };

  // The ground beside the grass under a root past an open edge. The root takes that ground's height where it goes
  // on from the grass's plane: no blade hangs over a ledge or stands in a wall
  std::vector<const WaterTri*> beside;
  if(!segments.empty()) {
    for(const Summary* s:in.summaries) {
      if(s==nullptr)
        continue;
      for(const auto& t:s->beside) {
        if(std::max({t.x[0],t.x[1],t.x[2]})<lo[0] || std::min({t.x[0],t.x[1],t.x[2]})>hi[0] ||
           std::max({t.z[0],t.z[1],t.z[2]})<lo[1] || std::min({t.z[0],t.z[1],t.z[2]})>hi[1])
          continue;
        beside.push_back(&t);
        }
      }
    }
  // w: the edge functions of the grass triangle the edge `s` belongs to, at the root outside it; d: cm past it
  auto pastEdge = [&](int32_t px, int32_t pz, int64_t w[3], int32_t& y, int32_t& d, const Segment*& s) -> const Ready* {
    if(beside.empty())
      return nullptr;
    const Segment* near = nullptr;
    const int32_t  cm   = edgeDistance(px,pz,PastEdge,near);
    if(near==nullptr || near->verge)
      return nullptr; // past a verge's border the cover decides
    const Ready& r = tris[near->tri];
    const Tri&   t = *r.t;
    for(int i=0; i<3; ++i) {
      const int a = (i+1)%3, b = (i+2)%3;
      w[i] = int64_t(t.x[b]-t.x[a])*int64_t(pz-t.z[a]) - int64_t(t.z[b]-t.z[a])*int64_t(px-t.x[a]);
      }
    const auto plane = int32_t((w[0]*t.y[0] + w[1]*t.y[1] + w[2]*t.y[2])/r.area2);
    int32_t off = PastStep+1;
    for(const WaterTri* g:beside) {
      int32_t x[3] = {g->x[0],g->x[1],g->x[2]}, z[3] = {g->z[0],g->z[1],g->z[2]};
      if(px<std::min({x[0],x[1],x[2]}) || px>std::max({x[0],x[1],x[2]}) ||
         pz<std::min({z[0],z[1],z[2]}) || pz>std::max({z[0],z[1],z[2]}) || !insideXZ(x,z,px,pz))
        continue;
      const int64_t area2 = int64_t(x[1]-x[0])*int64_t(z[2]-z[0]) - int64_t(x[2]-x[0])*int64_t(z[1]-z[0]);
      if(area2==0)
        continue;
      int64_t sum = 0;
      for(int i=0; i<3; ++i) {
        const int a = (i+1)%3, b = (i+2)%3;
        sum += (int64_t(x[b]-x[a])*int64_t(pz-z[a]) - int64_t(z[b]-z[a])*int64_t(px-x[a]))*int64_t(g->y[i]);
        }
      const auto h = int32_t(sum/area2);
      if(std::abs(h-plane)<std::abs(off)) {
        off = h-plane;
        y   = h;
        }
      }
    if(std::abs(off)>PastStep)
      return nullptr;
    d = cm;
    s = near;
    return &r;
    };

  // A root on a verge past the border `s`: the verge's own height under it, the nearest to the grass's plane there.
  // w: the edge functions of the grass triangle the border belongs to
  auto vergeRoot = [&](int32_t px, int32_t pz, const Segment& s, int64_t w[3], int32_t& y) -> const Ready* {
    const Ready& r = tris[s.tri];
    const Tri&   t = *r.t;
    for(int i=0; i<3; ++i) {
      const int a = (i+1)%3, b = (i+2)%3;
      w[i] = int64_t(t.x[b]-t.x[a])*int64_t(pz-t.z[a]) - int64_t(t.z[b]-t.z[a])*int64_t(px-t.x[a]);
      }
    const auto plane = int32_t((w[0]*t.y[0] + w[1]*t.y[1] + w[2]*t.y[2])/r.area2);
    bool    found = false;
    int32_t off   = 0;
    for(const VergeTri* g:verge) {
      if(g->slot!=t.slot ||
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
      const auto h = int32_t(sum/area2);
      if(!found || std::abs(h-plane)<std::abs(off)) {
        found = true;
        off   = h-plane;
        y     = h;
        }
      }
    return found ? &r : nullptr;
    };

  // the tufts round the cell: a jittered centre in each square of TuftStep, its patch and its own numbers
  const int32_t tx0 = floorDiv(lo[0],TuftStep)-2, tz0 = floorDiv(lo[1],TuftStep)-2;
  const int32_t tnx = floorDiv(hi[0],TuftStep)+3-tx0, tnz = floorDiv(hi[1],TuftStep)+3-tz0;
  std::vector<Tuft> tufts(size_t(tnx*tnz));
  for(int32_t j=0; j<tnz; ++j)
    for(int32_t i=0; i<tnx; ++i)
      tufts[size_t(j*tnx+i)] = Grass::tuft(tx0+i,tz0+j,in.patch);

  std::vector<Raw> raw;
  raw.reserve(size_t(CellNodes)*size_t(CellNodes)/4);
  int32_t minY = INT32_MAX, maxY = INT32_MIN;
  const int32_t i0 = in.cx*CellNodes, j0 = in.cz*CellNodes;
  for(int32_t j=j0; j<j0+CellNodes; ++j)
    for(int32_t i=i0; i<i0+CellNodes; ++i) {
      const uint64_t h = hash64(i,j,0x11u);
      const auto rank = uint32_t(h & 0xFFu);
      if(rank>=std::max(in.rankLimit,anyFine ? in.rankLimitFine : 0u))
        continue;
      const int32_t px = i*NodeStep + int32_t((h >> 8) & 63u), pz = j*NodeStep + int32_t((h >> 14) & 63u);

      // its tuft: the least measure of the 25 round it (a wide tuft reaches over its neighbour's square)
      const int32_t ti = floorDiv(px,TuftStep)-tx0, tj = floorDiv(pz,TuftStep)-tz0;
      const Tuft* tuft = nullptr;
      int64_t     near = INT64_MAX;
      for(int32_t dj=-2; dj<=2; ++dj)
        for(int32_t di=-2; di<=2; ++di) {
          const Tuft& t = tufts[size_t((tj+dj)*tnx + ti+di)];
          const int64_t d = tuftMeasure(t,px,pz);
          if(d<near) {
            near = d;
            tuft = &t;
            }
          }
      // keep by the patch: `roll` of 1024 against the share of the lattice's nodes the density asks for
      const auto roll = int64_t((h >> 20) & 1023u);
      // a fine blade is of the lattice's every node: the patch's count is not asked
      const bool kept = roll*NodesPerM2*256*256 < int64_t(tuft->densityQ8)*maxDensity*int64_t(BladesPerM2)*1024;
      if(!kept && !(anyFine && (tuft->sward || anyVerge)))
        continue;

      int64_t w[3];
      int32_t y = 0, past = -1; // cm past an open edge, or on the grass
      int32_t onVerge = -1;     // units past a border with a verge, on the verge
      const Segment* edge = nullptr;
      const Ready* at = locate(px,pz,w,y);
      if(at==nullptr && anyVerge) {
        const Segment* near = nullptr;
        const int32_t  d    = vergeDistance(px,pz,VergeBand,near);
        if(near!=nullptr && (at = vergeRoot(px,pz,*near,w,y))!=nullptr) {
          onVerge = d;
          edge    = near;
          }
        }
      if(at==nullptr && !segments.empty())
        at = pastEdge(px,pz,w,y,past,edge);
      if(at==nullptr)
        continue;
      const Slot&   slot = slots[at->t->slot];
      const Params& par  = in.params[at->t->slot];
      const bool    sward = par.cover && par.fine && (tuft->sward || onVerge>=0);
      if(!sward && (!kept || rank>=in.rankLimit))
        continue;
      // of a sward past its fine blades' ring a broad one alone may stand
      const bool mayBroad = sward && par.broad>0.f && kept && onVerge<0 && past<0 && rank<in.rankLimit;
      if(sward && !mayBroad && rank>=in.rankLimitFine)
        continue;
      if(at->tanQ12>=slot.tanMax)
        continue;
      const int32_t slopeQ8 = at->tanQ12<=slot.tanFull ? 256 :
                              int32_t(int64_t(slot.tanMax-at->tanQ12)*256/int64_t(slot.tanMax-slot.tanFull));
      // the band at an open edge: the distance with a noise of 1.5 m on it
      int32_t edgeQ8 = 256;
      if(past>=0 && slot.edgeCm<=0)
        continue;
      int32_t toEdge = INT32_MAX; // cm to an open edge, where it was asked
      if(past<0 && slot.edgeCm>0 && !segments.empty()) {
        // on a verge: as at the border itself
        toEdge = onVerge>=0 ? 0 : edgeDistance(px,pz,slot.edgeCm*2,edge);
        const int32_t n = valueNoise(px,pz,150*Unit,0x33u) - 32640;
        edgeQ8 = std::clamp((toEdge*256 + int32_t(int64_t(n)*slot.edgeCm*154/32640))/slot.edgeCm, 0, 256);
        }
      // 55 % of the patch's grass at the edge; past it from 45 % down to none at PastEdge
      int32_t edgeDensity = 141 + 115*edgeQ8/256;
      if(past>=0) {
        edgeQ8      = 0;
        edgeDensity = 115*(PastEdge-past)/PastEdge;
        }
      // a spot of bare earth in the sward: no blade in it, fewer and lower ones on its rim. With the cover that
      // is asked at the root below
      const int32_t bare  = (in.patch<0 && !par.cover) ? patchBare(px,pz) : 0;
      // of 2^40, the patch's count
      const int64_t shareBroad = int64_t(tuft->densityQ8)*slot.densityQ8*slopeQ8*edgeDensity*(256-bare);
      // gathered towards the tuft's centre, as far as this
      const int32_t pullBroad  = (past<0 && onVerge<0) ?
          std::min(int32_t(std::lround((20.f + float((h >> 30) & 31u))*par.clumps*2.56f))*tuft->pullQ8/256, 166) : 0;
      // A sward's broad blades among its fine ones (Params::broad): the blades the sward had before the fine ones,
      // a share of them that differs from tuft to tuft, so they stand in company. A short broad blade alone against
      // bare ground reads as a wedge: none stands on a verge, near a border or an open edge, where a spot's sward
      // thins or right beside that, and towards those a tuft's broad blades end all at the tuft's own line
      // (Tuft::gate), not one by one. Which a node is, it is at any distance: the two kinds do not change places
      bool broad = false;
      if(sward && par.broad>0.f && kept && past<0 && onVerge<0 &&
         roll*NodesPerM2*(int64_t(1) << 40) < shareBroad*int64_t(BladesPerM2)*1024) {
        // not by the rank: the kind is the node's
        const float   room  = std::min(par.broad,1.f-par.broad);
        int32_t       pick  = int32_t(std::lround((par.broad + (float(tuft->phase)/255.f-0.5f)*2.f*room)*65536.f));
        const int32_t dice  = int32_t(hash64(i,j,0x12u) & 0xFFFFu);
        // where its root would be
        const int32_t bx = px + int32_t(int64_t(tuft->x-px)*pullBroad/256);
        const int32_t bz = pz + int32_t(int64_t(tuft->z-pz)*pullBroad/256);
        int32_t clear = (bx<lo[0] || bx>=hi[0] || bz<lo[1] || bz>=hi[1]) ? 0 : 256;
        if(dice<pick && clear>0 && !segments.empty()) {
          const Segment* near = nullptr;
          const int32_t  d    = edgeDistance(bx,bz,CoverBroadTo,near);
          clear = std::clamp((d-CoverBroadFrom)*256/(CoverBroadTo-CoverBroadFrom), 0, 256);
          }
        if(dice<pick && clear>int32_t(tuft->gate) && in.patch<0)
          clear = std::min(clear,coverClear(bx,bz));
        broad = dice<pick && clear>int32_t(tuft->gate);
        }
      const bool fine = sward && !broad;
      if(fine ? rank>=in.rankLimitFine : rank>=in.rankLimit)
        continue;
      // of 2^40; a fine blade: the record's count to 1, of the lattice's every node
      const int64_t share = fine ? int64_t(std::min(slot.densityQ8,256))*slopeQ8*edgeDensity*(int64_t(1) << 16) : shareBroad;
      const int64_t nodes = fine ? int64_t(BladesPerM2) : NodesPerM2;
      if(roll*nodes*(int64_t(1) << 40) >= share*int64_t(BladesPerM2)*1024)
        continue;

      // the root must still stand on grass and out of the water. A root past an edge stays where its node put it
      int32_t pull = 0, fx = px, fz = pz;
      if(past<0 && onVerge<0) {
        pull = pullBroad;
        if(fine)
          pull /= 2; // a sward is a carpet more than it is tufts
        fx   = px + int32_t(int64_t(tuft->x-px)*pull/256);
        fz   = pz + int32_t(int64_t(tuft->z-pz)*pull/256);
        at   = locate(fx,fz,w,y);
        }
      if(at==nullptr || underWater(fx,fz,y))
        continue;
      const Tri&    t  = *at->t;
      const Params& fp = in.params[t.slot];
      // the cover at the root (grasscover.h): the spots, the border with a verge
      int32_t stand = 256;
      if(fp.cover) {
        int32_t border = 256;
        if(onVerge>=0) {
          border = coverBorder(fx,fz,onVerge,edge->reach);
          } else if(anyVerge) {
          const Segment* near = nullptr;
          const int32_t  d    = vergeDistance(fx,fz,CoverInside*Unit,near);
          if(near!=nullptr)
            border = coverBorder(fx,fz,-d,near->reach);
          }
        const int32_t level = in.patch<0 ? coverLevel(fx,fz,border) : border;
        if(broad && level<256)
          continue; // in whole sward only: never a thinned and lowered one
        stand = coverBlades(level);
        if(fine && in.patch<0)
          stand = coverFine(level,coverEdge(fx,fz,border));
        if(roll*nodes*(int64_t(1) << 40) >= (share >> 8)*stand*int64_t(BladesPerM2)*1024)
          continue;
        }
      const double  k  = 1.0/double(at->area2);
      const double  b0 = double(w[0])*k, b1 = double(w[1])*k, b2 = double(w[2])*k;

      // a sward's broad blade is whole or is not there: the edge's band neither lowers it nor leans it out
      const float edgeShare  = broad ? 1.f : float(edgeQ8)/256.f;
      const float slopeShare = float(slopeQ8)/256.f;
      const float fromCentre = float(std::sqrt(double(near)/65536.0))*(1.f-float(pull)/256.f)/float(TuftStep);
      float height = fp.height*tuft->height*(0.9f + tuft->spread*(float((h >> 35) & 0xFFu)/255.f - 0.5f));
      height *= 1.f - tuft->fall*std::min(fromCentre*1.6f,1.f);
      height *= 1.f - 0.5f*float(bare)/256.f;
      height *= 0.55f + 0.45f*float(stand)/256.f;
      height *= 0.5f + 0.5f*edgeShare;
      height *= 0.5f + 0.5f*slopeShare;
      if(past>=0)
        height = std::min(height*(1.f - 0.5f*float(past)/float(PastEdge)),PastHeight);
      if(onVerge>=0)
        height = std::min(height*(1.f - 0.4f*std::min(float(onVerge)/float(MaxVergeCm*Unit),1.f)),PastHeight);
      for(const Disc& d:in.items) {
        const double dx = double(d.x-fx)/Unit, dz = double(d.z-fz)/Unit;
        const double r  = std::sqrt(dx*dx+dz*dz);
        if(r<80.0)
          height = std::min(height, float(8.0 + (double(height)-8.0)*std::clamp((r-35.0)/45.0,0.0,1.0)));
        }
      height = std::clamp(height,2.f,63.f);

      // leans out of its tuft and to the tuft's own side, and over an open edge
      float dirX = float(fx-tuft->x), dirZ = float(fz-tuft->z);
      const float out1 = std::max(std::sqrt(dirX*dirX+dirZ*dirZ),1.f);
      dirX = dirX/out1*tuft->fan + tuft->leanX*(1.f-tuft->fan);
      dirZ = dirZ/out1*tuft->fan + tuft->leanZ*(1.f-tuft->fan);
      float angle = std::atan2(dirZ,dirX) + (float((h >> 43) & 0xFFu)/255.f-0.5f)*2.2f;
      float lean  = (10.f + 25.f*float((h >> 51) & 0x3Fu)/63.f*(0.4f + 0.6f*std::min(fromCentre*2.f,1.f)))*tuft->lean;
      if(fine) {
        // a fine blade stands nearly upright and faces where it will: fanned out of their tufts and leant over, the
        // blades of a short sward read as whorls of fur from above
        angle = float((h >> 43) & 0xFFu)*(6.2831853f/256.f);
        lean  = 8.f + 12.f*float((h >> 51) & 0x3Fu)/63.f;
        }
      if(edge!=nullptr && edgeShare<1.f) {
        const float out = std::atan2(edge->outZ,edge->outX);
        float d = angle-out;
        d -= 6.2831853f*std::floor(d/6.2831853f + 0.5f);
        angle = out + d*edgeShare;
        lean  = lean + (45.f-lean)*(1.f-edgeShare)*0.8f;
        }
      const float turn = angle/6.2831853f;

      Raw r;
      r.x      = fx;
      r.y      = y;
      r.z      = fz;
      r.slot   = t.slot;
      r.kind   = fine ? 1 : 0;
      r.rank   = uint8_t(rank);
      r.height = uint8_t(std::clamp(int(std::lround(height*4.f)),1,255));
      r.facing = uint8_t(int(std::floor((turn-std::floor(turn))*256.f)) & 0xFF);
      r.phase  = tuft->phase;
      r.lean   = uint8_t(std::clamp(int(std::lround((lean-8.f)*15.f/40.f)),0,15));
      // a blade of the sward is broad for its length, a blade of a tall clump is broad
      r.width  = fine ? uint8_t((h >> 57) & 7u) :
                 uint8_t(std::clamp(int((h >> 57) & 7u) + (tuft->clump ? 5 : (tuft->sward ? 3 : 0)),0,15));
      // a tuft is green or dry as one, its blades within a step of it; the fine ones differ blade from blade
      r.tint   = fine ? uint8_t(std::clamp(int(tuft->tint) + int((h >> 60) & 7u) - 3,0,15)) :
                 uint8_t(std::clamp(int(tuft->tint) + int((h >> 60) & 3u) - 1,0,15));
      const double nx = b0*t.nx[0] + b1*t.nx[1] + b2*t.nx[2];
      const double ny = b0*t.ny[0] + b1*t.ny[1] + b2*t.ny[2];
      const double nz = b0*t.nz[0] + b1*t.nz[1] + b2*t.nz[2];
      const double nl = std::max(std::sqrt(nx*nx+ny*ny+nz*nz),1e-6);
      r.nx     = uint8_t(std::clamp(int(std::lround(nx/nl*127.0))+128,1,255));
      r.nz     = uint8_t(std::clamp(int(std::lround(nz/nl*127.0))+128,1,255));
      const double u = b0*t.u[0] + b1*t.u[1] + b2*t.u[2], v = b0*t.v[0] + b1*t.v[1] + b2*t.v[2];
      r.u      = uint16_t(int(std::floor((u-std::floor(u))*4096.0)) & 0xFFF);
      r.v      = uint16_t(int(std::floor((v-std::floor(v))*4096.0)) & 0xFFF);
      raw.push_back(r);
      minY = std::min(minY,y);
      maxY = std::max(maxY,y);
      }
  if(raw.empty())
    return ret;

  // by slot, then by rank; the lattice's order within a rank
  std::stable_sort(raw.begin(),raw.end(),[](const Raw& a, const Raw& b) {
    return std::tie(a.slot,a.kind,a.rank)<std::tie(b.slot,b.kind,b.rank);
    });
  ret.minY = minY;
  ret.maxY = maxY;
  ret.blades.resize(raw.size());
  for(size_t i=0; i<raw.size(); ++i) {
    const Raw& r = raw[i];
    Blade& b = ret.blades[i];
    b.w[0] = uint32_t(r.x-lo[0]) | (uint32_t(r.z-lo[1]) << 14) | (uint32_t(r.lean) << 28);
    b.w[1] = uint32_t(std::min((r.y-minY)/2,0xFFFF)) | (uint32_t(r.height) << 16) | (uint32_t(r.rank) << 24);
    b.w[2] = uint32_t(r.facing) | (uint32_t(r.phase) << 8) | (uint32_t(r.nx) << 16) | (uint32_t(r.nz) << 24);
    b.w[3] = uint32_t(r.u) | (uint32_t(r.v) << 12) | (uint32_t(r.width) << 24) | (uint32_t(r.tint) << 28);
    if(ret.batches.empty() || ret.batches.back().slot!=r.slot || ret.batches.back().kind!=r.kind)
      ret.batches.push_back(Batch{r.slot,r.kind,uint32_t(i),0});
    ++ret.batches.back().count;
    }
  return ret;
  }

}
