#include "texeldensity.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Tempest;

TexelDensity TexelDensity::measure(const Resources::Vertex* vertices, size_t vertexCount, const uint32_t* indices,
                                   size_t first, size_t count, float metresPerUnit) {
  TexelDensity ret;
  // the threshold of the census (reports/engine/2026-11-10-texture-mips/census.py)
  constexpr double MinArea = 1e-4;
  double lowest = std::numeric_limits<double>::infinity();
  Vec3   lo( std::numeric_limits<float>::max());
  Vec3   hi(-std::numeric_limits<float>::max());
  const double scale = double(metresPerUnit);
  // Below this a triangle draws nothing at any distance (0.01 mm2); collinear corners leave rounding noise.
  constexpr double NoArea = 1e-8;
  size_t triangles = 0;
  bool   drawn     = false;
  for(size_t i=first; i+2<first+count; i+=3) {
    const uint32_t ia = indices[i+0], ib = indices[i+1], ic = indices[i+2];
    if(ia>=vertexCount || ib>=vertexCount || ic>=vertexCount)
      continue;
    const auto& a = vertices[ia];
    const auto& b = vertices[ib];
    const auto& c = vertices[ic];
    double e1[3], e2[3];
    for(int k=0; k<3; ++k) {
      e1[k] = (double(b.pos[k])-double(a.pos[k]))*scale;
      e2[k] = (double(c.pos[k])-double(a.pos[k]))*scale;
      }
    const double nx = e1[1]*e2[2]-e1[2]*e2[1];
    const double ny = e1[2]*e2[0]-e1[0]*e2[2];
    const double nz = e1[0]*e2[1]-e1[1]*e2[0];
    const double area = 0.5*std::sqrt(nx*nx+ny*ny+nz*nz);
    ++triangles;
    drawn = drawn || !(area<=NoArea);
    const double u1 = double(b.uv[0])-double(a.uv[0]), v1 = double(b.uv[1])-double(a.uv[1]);
    const double u2 = double(c.uv[0])-double(a.uv[0]), v2 = double(c.uv[1])-double(a.uv[1]);
    const double uvArea = 0.5*std::abs(u1*v2-v1*u2);
    if(!(area>MinArea) || !(uvArea>0) || !std::isfinite(area) || !std::isfinite(uvArea))
      continue;
    lowest = std::min(lowest,std::sqrt(uvArea/area));
    for(const auto* v:{&a,&b,&c}) {
      lo.x = std::min(lo.x,v->pos[0]); hi.x = std::max(hi.x,v->pos[0]);
      lo.y = std::min(lo.y,v->pos[1]); hi.y = std::max(hi.y,v->pos[1]);
      lo.z = std::min(lo.z,v->pos[2]); hi.z = std::max(hi.z,v->pos[2]);
      }
    }
  if(!std::isfinite(lowest) || !(lowest>0) || lowest>double(std::numeric_limits<float>::max())) {
    ret.flat = triangles>0 && !drawn;
    return ret;
    }
  ret.uvPerMetre = float(lowest);
  ret.min = lo;
  ret.max = hi;
  return ret;
  }

float TexelDensity::scaleOf(const Matrix4x4& m) {
  float axis = 0;
  for(int i=0; i<3; ++i) {
    const float x = m.at(i,0), y = m.at(i,1), z = m.at(i,2);
    axis = std::max(axis,std::sqrt(x*x+y*y+z*z));
    }
  return axis>0 && std::isfinite(axis) ? axis : 0.f;
  }

TexelDensity TexelDensity::merged(const TexelDensity& other) const {
  if(!valid()) {
    TexelDensity ret = other;
    ret.flat = !other.valid() && flat && other.flat;
    return ret;
    }
  if(!other.valid())
    return *this;
  TexelDensity ret;
  ret.uvPerMetre = std::min(uvPerMetre,other.uvPerMetre);
  ret.min = Vec3(std::min(min.x,other.min.x),std::min(min.y,other.min.y),std::min(min.z,other.min.z));
  ret.max = Vec3(std::max(max.x,other.max.x),std::max(max.y,other.max.y),std::max(max.z,other.max.z));
  return ret;
  }

float TexelDensity::radius() const {
  if(!valid())
    return 0;
  const float x = std::max(std::abs(min.x),std::abs(max.x));
  const float y = std::max(std::abs(min.y),std::abs(max.y));
  const float z = std::max(std::abs(min.z),std::abs(max.z));
  return std::sqrt(x*x+y*y+z*z);
  }

TexelDensity TexelDensity::placed(const Matrix4x4& m) const {
  TexelDensity ret;
  ret.flat = flat;
  if(!valid())
    return ret;
  const float axis = scaleOf(m);
  if(!(axis>0))
    return ret;
  Vec3 lo( std::numeric_limits<float>::max());
  Vec3 hi(-std::numeric_limits<float>::max());
  for(int i=0; i<8; ++i) {
    Vec3 p((i&1) ? max.x : min.x, (i&2) ? max.y : min.y, (i&4) ? max.z : min.z);
    m.project(p);
    lo.x = std::min(lo.x,p.x); hi.x = std::max(hi.x,p.x);
    lo.y = std::min(lo.y,p.y); hi.y = std::max(hi.y,p.y);
    lo.z = std::min(lo.z,p.z); hi.z = std::max(hi.z,p.z);
    }
  ret.uvPerMetre = uvPerMetre/axis;
  ret.min = lo;
  ret.max = hi;
  return ret;
  }
