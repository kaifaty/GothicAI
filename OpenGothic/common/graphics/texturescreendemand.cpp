#include "texturescreendemand.h"

#include <algorithm>
#include <cmath>

double TextureScreenDemand::distance(const Tempest::Vec3& p, const TexelDensity& texel) {
  const double x = std::max({double(texel.min.x)-p.x,0.0,double(p.x)-texel.max.x});
  const double y = std::max({double(texel.min.y)-p.y,0.0,double(p.y)-texel.max.y});
  const double z = std::max({double(texel.min.z)-p.z,0.0,double(p.z)-texel.max.z});
  return std::sqrt(x*x+y*y+z*z)*0.01;
  }

double TextureScreenDemand::distance(const Tempest::Vec3& p, const Sphere& sphere) {
  const double x = double(p.x)-sphere.centre.x, y = double(p.y)-sphere.centre.y, z = double(p.z)-sphere.centre.z;
  return std::max(std::sqrt(x*x+y*y+z*z)-double(sphere.radius),0.0)*0.01;
  }

namespace {
template<class Distance>
TextureScreenDemand::Result evaluateAt(const TextureScreenDemand::View& view, const TexelDensity& texel, int32_t previous,
                                       Distance distance) {
  TextureScreenDemand::Result ret;
  if(!texel.valid()) {
    ret.exponent = texel.flat ? TextureScreenDemand::Coarsest : TextureScreenDemand::WholeChain;
    return ret;
    }
  if(!view.known) {
    ret.expiry    = -1;
    ret.scheduled = true;
    return ret;
    }
  const double d = std::min(distance(view.hero),distance(view.camera));
  const auto demand = TextureLevels::demand(d,texel.uvPerMetre,
                                            previous==TextureScreenDemand::WholeChain ? TextureLevels::NoExponent : previous,
                                            view.coarsest);
  if(demand.exponent==TextureLevels::NoExponent) {
    ret.exponent = TextureScreenDemand::WholeChain;
    return ret;
    }
  ret.exponent  = demand.exponent;
  ret.expiry    = view.odometer+demand.margin;
  ret.margin    = demand.margin;
  ret.scheduled = true;
  return ret;
  }
}

TextureScreenDemand::Result TextureScreenDemand::evaluate(const View& view, const TexelDensity& texel, int32_t previous) {
  return evaluateAt(view,texel,previous,[&](const Tempest::Vec3& p) { return distance(p,texel); });
  }

TextureScreenDemand::Result TextureScreenDemand::evaluate(const View& view, const TexelDensity& texel, const Sphere& bounds,
                                                          int32_t previous) {
  return evaluateAt(view,texel,previous,[&](const Tempest::Vec3& p) { return distance(p,bounds); });
  }

namespace {
bool later(const TextureScreenDemand::Entry& a, const TextureScreenDemand::Entry& b) {
  return a.expiry>b.expiry;
  }
}

void TextureScreenDemand::push(size_t owner, double expiry, uint64_t stamp) {
  heap.push_back(Entry{expiry,owner,stamp});
  std::push_heap(heap.begin(),heap.end(),later);
  }

bool TextureScreenDemand::due(double odometer, Entry& out) const {
  if(heap.empty() || heap.front().expiry>odometer)
    return false;
  out = heap.front();
  return true;
  }

void TextureScreenDemand::pop() {
  std::pop_heap(heap.begin(),heap.end(),later);
  heap.pop_back();
  }

void TextureScreenDemand::rebuild() {
  std::make_heap(heap.begin(),heap.end(),later);
  }
