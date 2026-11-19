#include "arrowdepth.h"

#include <algorithm>
#include <cmath>

#include "graphics/mesh/protomesh.h"

using namespace Tempest;

namespace {

Vec3 column(const Matrix4x4& m, int c) {
  return Vec3(m.at(c,0),m.at(c,1),m.at(c,2));
  }

bool finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  }

}

float ArrowDepth::world(asset::MaterialGroup group) {
  switch(group) {
    case asset::MaterialGroup::WOOD:
      return 8.f;
    case asset::MaterialGroup::STONE:
    case asset::MaterialGroup::METAL:
      return 2.f;
    default:
      // EARTH, SNOW, UNDEFINED and the rest
      return 15.f;
    }
  }

float ArrowDepth::tipLength(const ProtoMesh* mesh) {
  if(mesh==nullptr)
    return 0;
  const auto* box = mesh->bbox();
  if(box==nullptr || !finite(box[0]) || !finite(box[1]) || !(box[1].x>box[0].x))
    return 0;
  const float length = -box[0].x;
  return length>0.f ? length : 0.f;
  }

void ArrowDepth::faceSurface(Matrix4x4& m, const Vec3& into, const Vec3& inward) {
  const auto tip = -column(m,0);
  if(Vec3::dotProduct(tip,inward)>0.f || into.length()<=1.e-6f || !finite(into))
    return;
  // the same frame a flying projectile has (DynamicWorld::BulletBody::matrix), along `into`
  const auto d   = Vec3::normalize(into);
  const auto pos = column(m,3);
  const float a2  = std::asin(std::clamp(d.y,-1.f,1.f))*float(180/M_PI);
  const float ang = std::atan2(d.z,d.x)*float(180/M_PI)+180.f;
  m.identity();
  m.translate(pos.x,pos.y,pos.z);
  m.rotateOY(-ang);
  m.rotateOZ(-a2);
  }

void ArrowDepth::sink(Matrix4x4& m, const Vec3& entry, float depth, float tipLength) {
  const auto x = column(m,0);
  const float scale = x.length();
  if(!(tipLength>0.f) || !(scale>1.e-6f) || !finite(entry))
    return;
  // the tip is `tipLength` along −X of the mesh: put it `depth` past the entry along the flight
  const auto flight = -x/scale;
  const auto origin = entry + flight*(depth-tipLength*scale);
  m.set(3,0,origin.x);
  m.set(3,1,origin.y);
  m.set(3,2,origin.z);
  }
