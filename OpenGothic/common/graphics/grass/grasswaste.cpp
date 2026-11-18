#include "grasswaste.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "graphics/dynamic/frustrum.h"
#include "graphics/grass/grassdiag.h"

using namespace Tempest;

namespace Grass {

bool wasteOn() {
  static const bool value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_WASTE");
    return e!=nullptr && e[0]=='1' && e[1]=='\0';
    }();
  return value;
  }

size_t WasteRule::ranks(float dist) const {
  return size_t(std::min(256.f, std::ceil(256.f*rankShare*full/std::max(dist-rootsBeyond,1.f))));
  }

size_t wasteBand(float dist) {
  size_t band = 0;
  for(float to:Waste::BandCm)
    if(dist>=to)
      ++band;
  return band;
  }

uint8_t wasteLinks(size_t set, float cellDist) {
  uint8_t links = MaxSegments;
  for(float to:Waste::LinkCm[set])
    if(cellDist>=to)
      --links;
  return links;
  }

uint8_t WasteRule::links(float dist) const {
  if(fine)
    return uint8_t(dist<full ? 2 : 1);
  return broadLinks(dist);
  }

void countWaste(Waste& out, const Frustrum& fr, const Vec3& cam, const WasteRule& rule, const WasteCell& cell,
                std::span<const Blade> drawn, uint8_t links) {
  if(drawn.empty())
    return;
  // the squares: in view or not, the ranks and the links at the nearest of each
  struct Sub final {
    bool     on    = false;
    bool     used  = false;
    uint32_t ranks = 0;
    uint8_t  links = 0;
    };
  constexpr size_t MaxN = 8;
  Sub     sub[Waste::Subs][MaxN*MaxN];
  int32_t subN[Waste::Subs] = {};
  for(size_t s=0; s<Waste::Subs; ++s) {
    const float   step = Waste::SubCm[s];
    const int32_t n    = std::min(int32_t(MaxN), std::max(1, int32_t(std::lround(cell.size/step))));
    subN[s] = n;
    for(int32_t i=0; i<n; ++i)
      for(int32_t j=0; j<n; ++j) {
        const float x0 = cell.x0 + float(i)*step, z0 = cell.z0 + float(j)*step;
        const float dx = std::max(0.f, std::max(x0 - cam.x, cam.x - (x0+step)));
        const float dz = std::max(0.f, std::max(z0 - cam.z, cam.z - (z0+step)));
        const float dist = std::sqrt(dx*dx + dz*dz);
        auto& q = sub[s][size_t(i*n+j)];
        if(dist>rule.cellRadius || (rule.fine && dist>=rule.radius))
          continue;
        const Vec3 min(x0-cell.pad,      cell.minY, z0-cell.pad);
        const Vec3 max(x0+step+cell.pad, cell.maxY, z0+step+cell.pad);
        if(fr.testBbox(min,max)==Frustrum::T_Invisible)
          continue;
        q.on    = true;
        q.ranks = uint32_t(rule.ranks(dist));
        q.links = rule.links(dist);
        }
    }

  const size_t tris   = size_t(2*links-1);
  const float  margin = float(CellMargin)/float(Unit);
  size_t setTris[Waste::LinkSets] = {};
  for(size_t s=0; s<Waste::LinkSets; ++s)
    setTris[s] = rule.fine ? tris : size_t(2*wasteLinks(s,cell.dist)-1);
  for(const auto& b:drawn) {
    const float lx = float(b.w[0] & 0x3FFFu)/float(Unit), lz = float((b.w[0] >> 14) & 0x3FFFu)/float(Unit);
    const Vec3  root(cell.origin.x + lx, cell.origin.y + float((b.w[1] & 0xFFFFu)*2u)/float(Unit), cell.origin.z + lz);
    const uint32_t r    = b.w[1] >> 24;
    const float    rank = (float(r)+0.5f)/256.f;
    float          h    = float((b.w[1] >> 16) & 0xFFu)*0.25f;

    // grass.vert
    const float d    = (root-cam).length();
    const float grow = std::clamp((rule.full/std::max(d,1.f)*1.12f - rank)/0.12f, 0.f, 1.f);
    const float t    = std::clamp((d-rule.fadeFrom)/(rule.radius-rule.fadeFrom), 0.f, 1.f);
    h *= grow*(1.f - t*t*(3.f-2.f*t));
    bool shown = false;
    if(h<=0.05f) {
      ++out.gone;
      out.goneTriangles += tris;
      }
    else if(!fr.testPoint(root.x, root.y+0.5f*h, root.z, 0.5f*h+20.f)) {
      ++out.out;
      out.outTriangles += tris;
      }
    else
      shown = true;

    const size_t band = wasteBand(d);
    ++out.bandBlades[band];
    out.bandFine[band]  += rule.fine ? 1 : 0;
    out.bandShown[band] += shown ? 1 : 0;
    for(size_t s=0; s<Waste::LinkSets; ++s) {
      out.bandTriangles[s][band]      += setTris[s];
      out.bandShownTriangles[s][band] += shown ? setTris[s] : 0;
      }

    for(size_t s=0; s<Waste::Subs; ++s) {
      const int32_t n    = subN[s];
      const float   step = Waste::SubCm[s];
      const int32_t i    = std::clamp(int32_t(std::floor((lx-margin)/step)), 0, n-1);
      const int32_t j    = std::clamp(int32_t(std::floor((lz-margin)/step)), 0, n-1);
      auto&         q    = sub[s][size_t(i*n+j)];
      if(!q.on || r>=q.ranks)
        continue;
      ++out.subDrawn[s];
      out.subTriangles[s] += size_t(2*q.links-1);
      if(!q.used) {
        q.used = true;
        ++out.subDraws[s];
        }
      }
    }
  }

}
