#include "grassplace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace Grass {

namespace {

int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a/b;
  return (a%b!=0 && ((a<0)!=(b<0))) ? q-1 : q;
  }

// The same hash as grassHash of grass_common.glsl: 32 bits, so the ground's shader repeats the patches
uint32_t hash32(int32_t ix, int32_t iz, uint32_t salt) {
  uint32_t h = uint32_t(ix)*0x9E3779B1u ^ uint32_t(iz)*0x85EBCA77u ^ salt;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
  }

uint64_t hash64(int32_t a, int32_t b, uint32_t salt) {
  uint64_t x = (uint64_t(uint32_t(a)) << 32 | uint64_t(uint32_t(b))) ^ (uint64_t(salt)*0x9E3779B97F4A7C15ull);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
  }

// Value noise on a lattice of `step` units: 0..65280
int32_t valueNoise(int32_t x, int32_t z, int32_t step, uint32_t salt) {
  const int32_t ix = floorDiv(x,step), iz = floorDiv(z,step);
  const int32_t tx = ((x-ix*step)*256)/step, tz = ((z-iz*step)*256)/step;
  const int32_t sx = (tx*tx*(768-2*tx)) >> 16, sz = (tz*tz*(768-2*tz)) >> 16;
  auto at = [&](int32_t i, int32_t j) { return int32_t((hash32(i,j,salt) >> 8) & 0xFFu); };
  const int32_t top = at(ix,iz)  *(256-sx) + at(ix+1,iz)  *sx;
  const int32_t bot = at(ix,iz+1)*(256-sx) + at(ix+1,iz+1)*sx;
  return (top*(256-sz) + bot*sz) >> 8;
  }

uint32_t isqrt(uint64_t v) {
  auto r = uint64_t(std::sqrt(double(v)));
  while(r*r>v)
    --r;
  while((r+1)*(r+1)<=v)
    ++r;
  return uint32_t(r);
  }

// The patches by the noise's value: its measured quantiles 7, 11, 30, 38 and 88 % and the top of the clumps
constexpr int32_t PatchBare  = 15962;
constexpr int32_t PatchShort = 18361;
constexpr int32_t PatchMain0 = 26192;
constexpr int32_t PatchMain1 = 28857;
constexpr int32_t PatchClump = 46436;
constexpr int32_t PatchTop   = 56000;

int32_t ramp(int32_t n, int32_t a, int32_t b, int32_t va, int32_t vb) {
  return va + int32_t(int64_t(vb-va)*int64_t(n-a)/int64_t(b-a));
  }

float rampF(int32_t n, int32_t a, int32_t b, float va, float vb) {
  return va + (vb-va)*float(n-a)/float(b-a);
  }

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

bool insideXZ(int32_t x[3], int32_t z[3], int32_t px, int32_t pz) {
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

int32_t units(float cm) {
  return int32_t(std::lround(double(cm)*Unit));
  }

struct Segment final { // an open edge: grass on its left only
  int32_t  ax = 0, az = 0, bx = 0, bz = 0; // cm
  float    outX = 0.f, outZ = 0.f;
  uint32_t tri = 0;                        // the triangle it is an edge of
  };

struct Raw final { // a blade before it is packed
  int32_t  x = 0, y = 0, z = 0;
  uint16_t slot = 0;
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

int32_t patchNoise(int32_t x, int32_t z) {
  return (3*valueNoise(x,z,600*Unit,0x51u) + valueNoise(x+1777,z+911,220*Unit,0xA7u)) >> 2;
  }

int32_t patchDensity(int32_t n) {
  if(n<PatchBare)
    return 0;
  if(n<PatchShort)
    return ramp(n,PatchBare,PatchShort,0,205);
  if(n<PatchMain0)
    return 205;
  if(n<PatchMain1)
    return ramp(n,PatchMain0,PatchMain1,205,256);
  if(n<PatchClump)
    return 256;
  if(n<PatchTop)
    return ramp(n,PatchClump,PatchTop,256,512);
  return 512;
  }

float patchHeight(int32_t n) {
  if(n<PatchShort)
    return 0.45f;
  if(n<PatchMain0)
    return rampF(n,PatchShort,PatchMain0,0.45f,0.65f);
  if(n<PatchMain1)
    return rampF(n,PatchMain0,PatchMain1,0.65f,0.9f);
  if(n<PatchClump)
    return rampF(n,PatchMain1,PatchClump,0.9f,1.15f);
  if(n<PatchTop)
    return rampF(n,PatchClump,PatchTop,1.5f,2.4f);
  return 2.4f;
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
  for(size_t i=0; i<slots.size(); ++i) {
    const auto& p = in.params[i];
    if(!p.on)
      continue;
    slots[i].densityQ8 = int32_t(std::lround(double(p.density)*double(in.density)*256.0));
    slots[i].tanMax    = int32_t(std::lround(std::tan(double(p.slope)*M_PI/180.0)*4096.0));
    slots[i].tanFull   = int32_t(std::lround(std::tan(double(std::max(p.slope-10.f,0.f))*M_PI/180.0)*4096.0));
    slots[i].edgeCm    = int32_t(std::lround(p.edge));
    maxEdge    = std::max(maxEdge,slots[i].edgeCm);
    maxDensity = std::max(maxDensity,slots[i].densityQ8);
    }
  if(maxDensity<=0)
    return ret;
  // an open edge is felt to twice the record's band (the noise on the distance)
  const int32_t reach = CellMargin + (2*maxEdge+100)*Unit;

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
  std::sort(tris.begin(),tris.end(),[](const Ready& a, const Ready& b) {
    const auto& l = *a.t; const auto& r = *b.t;
    return std::tie(l.x[0],l.z[0],l.y[0],l.x[1],l.z[1],l.y[1],l.x[2],l.z[2],l.y[2],l.slot) <
           std::tie(r.x[0],r.z[0],r.y[0],r.x[1],r.z[1],r.y[1],r.x[2],r.z[2],r.y[2],r.slot);
    });

  // bins over the cell and its margin: the triangles and the open edges near each
  constexpr int32_t Bins = 16;
  const int32_t binSize = (hi[0]-lo[0]+Bins-1)/Bins;
  std::array<std::vector<uint32_t>,Bins*Bins> triBins, segBins;
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
  auto onGrass = [&](int32_t px, int32_t pz) {
    for(const Ready& r:tris) {
      if(px<r.minX || px>r.maxX || pz<r.minZ || pz>r.maxZ)
        continue;
      int32_t x[3] = {r.t->x[0],r.t->x[1],r.t->x[2]}, z[3] = {r.t->z[0],r.t->z[1],r.t->z[2]};
      if(insideXZ(x,z,px,pz))
        return true;
      }
    return false;
    };

  // Open edges: an edge one triangle has, whose pieces of 50 cm have no grass 6 cm outside (so an edge that meets
  // two shorter ones of its neighbours is not open). Neighbouring open pieces are joined
  std::vector<Segment> segments;
  if(maxEdge>0) {
    struct Edge final {
      int32_t a[3], b[3]; // sorted ends
      int32_t from[2], to[2]; // as the triangle goes round
      uint32_t tri;
      };
    std::vector<Edge> edges;
    edges.reserve(tris.size()*3);
    for(size_t id=0; id<tris.size(); ++id) {
      const auto& t = *tris[id].t;
      for(int i=0; i<3; ++i) {
        const int j = (i+1)%3;
        Edge e;
        e.tri = uint32_t(id);
        e.from[0] = t.x[i]; e.from[1] = t.z[i]; e.to[0] = t.x[j]; e.to[1] = t.z[j];
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
    const int32_t band = 2*maxEdge*Unit;
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
      const double ox = dz/len, oz = -dx/len;
      const int    pieces = std::max(1,int(std::ceil(len/(50.0*Unit))));
      int          start  = -1;
      for(int k=0; k<=pieces; ++k) {
        bool open = false;
        if(k<pieces) {
          const double m = (double(k)+0.5)/double(pieces);
          const auto px = int32_t(std::lround(double(e.from[0]) + dx*m + ox*6.0*Unit));
          const auto pz = int32_t(std::lround(double(e.from[1]) + dz*m + oz*6.0*Unit));
          open = !onGrass(px,pz);
          }
        if(open && start<0)
          start = k;
        if(!open && start>=0) {
          Segment s;
          s.ax = int32_t(std::lround((double(e.from[0]) + dx*double(start)/double(pieces))/Unit));
          s.az = int32_t(std::lround((double(e.from[1]) + dz*double(start)/double(pieces))/Unit));
          s.bx = int32_t(std::lround((double(e.from[0]) + dx*double(k)/double(pieces))/Unit));
          s.bz = int32_t(std::lround((double(e.from[1]) + dz*double(k)/double(pieces))/Unit));
          s.outX = float(ox);
          s.outZ = float(oz);
          s.tri  = e.tri;
          segments.push_back(s);
          start = -1;
          }
        }
      }
    for(size_t i=0; i<segments.size(); ++i) {
      const auto& s = segments[i];
      forBins(std::min(s.ax,s.bx)*Unit-band, std::max(s.ax,s.bx)*Unit+band,
              std::min(s.az,s.bz)*Unit-band, std::max(s.az,s.bz)*Unit+band,
              [&](size_t b) { segBins[b].push_back(uint32_t(i)); });
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
    if(near==nullptr)
      return nullptr;
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

  // the tufts round the cell: a jittered centre in each square of TuftStep, its patch and its own numbers
  struct Tuft final {
    int32_t  x = 0, z = 0;
    int32_t  densityQ8 = 0;
    float    height = 1.f;
    uint64_t hash = 0;
    };
  const int32_t tx0 = floorDiv(lo[0],TuftStep)-1, tz0 = floorDiv(lo[1],TuftStep)-1;
  const int32_t tnx = floorDiv(hi[0],TuftStep)+2-tx0, tnz = floorDiv(hi[1],TuftStep)+2-tz0;
  std::vector<Tuft> tufts(size_t(tnx*tnz));
  for(int32_t j=0; j<tnz; ++j)
    for(int32_t i=0; i<tnx; ++i) {
      Tuft& t = tufts[size_t(j*tnx+i)];
      t.hash = hash64(tx0+i,tz0+j,0x7Fu);
      t.x    = (tx0+i)*TuftStep + int32_t(t.hash % uint64_t(TuftStep));
      t.z    = (tz0+j)*TuftStep + int32_t((t.hash >> 16) % uint64_t(TuftStep));
      const int32_t n = patchNoise(t.x,t.z);
      t.densityQ8 = patchDensity(n);
      t.height    = patchHeight(n)*(0.8f + 0.45f*float((t.hash >> 32) & 0xFFu)/255.f);
      }

  std::vector<Raw> raw;
  raw.reserve(size_t(CellNodes)*size_t(CellNodes)/4);
  int32_t minY = INT32_MAX, maxY = INT32_MIN;
  const int32_t i0 = in.cx*CellNodes, j0 = in.cz*CellNodes;
  for(int32_t j=j0; j<j0+CellNodes; ++j)
    for(int32_t i=i0; i<i0+CellNodes; ++i) {
      const uint64_t h = hash64(i,j,0x11u);
      const auto rank = uint32_t(h & 0xFFu);
      if(rank>=in.rankLimit)
        continue;
      const int32_t px = i*NodeStep + int32_t((h >> 8) & 63u), pz = j*NodeStep + int32_t((h >> 14) & 63u);

      // its tuft: the nearest centre
      const int32_t ti = floorDiv(px,TuftStep)-tx0, tj = floorDiv(pz,TuftStep)-tz0;
      const Tuft* tuft = nullptr;
      int64_t     near = INT64_MAX;
      for(int32_t dj=-1; dj<=1; ++dj)
        for(int32_t di=-1; di<=1; ++di) {
          const Tuft& t = tufts[size_t((tj+dj)*tnx + ti+di)];
          const int64_t d = int64_t(t.x-px)*int64_t(t.x-px) + int64_t(t.z-pz)*int64_t(t.z-pz);
          if(d<near) {
            near = d;
            tuft = &t;
            }
          }
      // keep by the patch: `roll` of 1024 against the share of the lattice's nodes the density asks for
      const auto roll = int64_t((h >> 20) & 1023u);
      if(roll*NodesPerM2*256*256 >= int64_t(tuft->densityQ8)*maxDensity*int64_t(BladesPerM2)*1024)
        continue;

      int64_t w[3];
      int32_t y = 0, past = -1; // cm past an open edge, or on the grass
      const Segment* edge = nullptr;
      const Ready* at = locate(px,pz,w,y);
      if(at==nullptr && !segments.empty())
        at = pastEdge(px,pz,w,y,past,edge);
      if(at==nullptr)
        continue;
      const Slot&   slot = slots[at->t->slot];
      const Params& par  = in.params[at->t->slot];
      if(at->tanQ12>=slot.tanMax)
        continue;
      const int32_t slopeQ8 = at->tanQ12<=slot.tanFull ? 256 :
                              int32_t(int64_t(slot.tanMax-at->tanQ12)*256/int64_t(slot.tanMax-slot.tanFull));
      // the band at an open edge: the distance with a noise of 1.5 m on it
      int32_t edgeQ8 = 256;
      if(past>=0 && slot.edgeCm<=0)
        continue;
      if(past<0 && slot.edgeCm>0 && !segments.empty()) {
        const int32_t d = edgeDistance(px,pz,slot.edgeCm*2,edge);
        const int32_t n = valueNoise(px,pz,150*Unit,0x33u) - 32640;
        edgeQ8 = std::clamp((d*256 + int32_t(int64_t(n)*slot.edgeCm*154/32640))/slot.edgeCm, 0, 256);
        }
      // 55 % of the patch's grass at the edge; past it from 45 % down to none at PastEdge
      int32_t edgeDensity = 141 + 115*edgeQ8/256;
      if(past>=0) {
        edgeQ8      = 0;
        edgeDensity = 115*(PastEdge-past)/PastEdge;
        }
      const int64_t share = int64_t(tuft->densityQ8)*slot.densityQ8*slopeQ8*edgeDensity;
      if(roll*NodesPerM2*(int64_t(1) << 32) >= share*int64_t(BladesPerM2)*1024)
        continue;

      // gathered towards the tuft's centre; the root must still stand on grass and out of the water. A root past
      // an edge stays where its node put it
      int32_t pull = 0, fx = px, fz = pz;
      if(past<0) {
        pull = int32_t(std::lround((20.f + float((h >> 30) & 31u))*par.clumps*2.56f));
        fx   = px + int32_t(int64_t(tuft->x-px)*pull/256);
        fz   = pz + int32_t(int64_t(tuft->z-pz)*pull/256);
        at   = locate(fx,fz,w,y);
        }
      if(at==nullptr || underWater(fx,fz,y))
        continue;
      const Tri&    t  = *at->t;
      const Params& fp = in.params[t.slot];
      const double  k  = 1.0/double(at->area2);
      const double  b0 = double(w[0])*k, b1 = double(w[1])*k, b2 = double(w[2])*k;

      const float edgeShare  = float(edgeQ8)/256.f;
      const float slopeShare = float(slopeQ8)/256.f;
      const float fromCentre = float(std::sqrt(double(near)))*(1.f-float(pull)/256.f)/float(TuftStep);
      float height = fp.height*tuft->height*(0.65f + 0.5f*float((h >> 35) & 0xFFu)/255.f);
      height *= 1.f - 0.4f*std::min(fromCentre*1.6f,1.f);
      height *= 0.5f + 0.5f*edgeShare;
      height *= 0.5f + 0.5f*slopeShare;
      if(past>=0)
        height = std::min(height*(1.f - 0.5f*float(past)/float(PastEdge)),PastHeight);
      for(const Disc& d:in.items) {
        const double dx = double(d.x-fx)/Unit, dz = double(d.z-fz)/Unit;
        const double r  = std::sqrt(dx*dx+dz*dz);
        if(r<80.0)
          height = std::min(height, float(8.0 + (double(height)-8.0)*std::clamp((r-35.0)/45.0,0.0,1.0)));
        }
      height = std::clamp(height,2.f,63.f);

      // leans out of its tuft, and over an open edge
      float dirX = float(fx-tuft->x), dirZ = float(fz-tuft->z);
      float angle = std::atan2(dirZ,dirX) + (float((h >> 43) & 0xFFu)/255.f-0.5f)*2.2f;
      float lean  = 10.f + 25.f*float((h >> 51) & 0x3Fu)/63.f*(0.4f + 0.6f*std::min(fromCentre*2.f,1.f));
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
      r.rank   = uint8_t(rank);
      r.height = uint8_t(std::clamp(int(std::lround(height*4.f)),1,255));
      r.facing = uint8_t(int(std::floor((turn-std::floor(turn))*256.f)) & 0xFF);
      r.phase  = uint8_t((tuft->hash >> 40) & 0xFFu);
      r.lean   = uint8_t(std::clamp(int(std::lround((lean-8.f)*15.f/40.f)),0,15));
      const bool clump = tuft->densityQ8>256;
      r.width  = uint8_t(std::clamp(int((h >> 57) & 7u) + (clump ? 6 : 0),0,15));
      r.tint   = uint8_t(std::clamp(int((tuft->hash >> 48) & 15u) + int((h >> 60) & 3u) - 1,0,15));
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
    return std::tie(a.slot,a.rank)<std::tie(b.slot,b.rank);
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
    if(ret.batches.empty() || ret.batches.back().slot!=r.slot)
      ret.batches.push_back(Batch{r.slot,uint32_t(i),0});
    ++ret.batches.back().count;
    }
  return ret;
  }

}
