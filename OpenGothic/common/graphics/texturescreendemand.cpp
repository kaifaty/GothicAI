#include "texturescreendemand.h"
#include "texturelookahead.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

uint32_t TextureScreenDemand::distanceClass(double d, uint32_t previous, double& margin) {
  uint32_t c = d<ClassBorder[0] ? 0 : d<ClassBorder[1] ? 1 : 2;
  if(previous<c && d<ClassBorder[previous]*(1.0+ClassHysteresis))
    c = previous;
  const double inf  = std::numeric_limits<double>::infinity();
  const double nearer  = c==0 ? inf : d-ClassBorder[c-1];
  const double farther = c==2 ? inf : ClassBorder[c]*(1.0+ClassHysteresis)-d;
  margin = std::min(nearer,farther);
  return c;
  }

bool TextureScreenDemand::View::lookAhead() const {
  return heroAhead.x!=0 || heroAhead.y!=0 || heroAhead.z!=0 || cameraAhead.x!=0 || cameraAhead.y!=0 || cameraAhead.z!=0;
  }

namespace {
template<class Distance, class Path>
TextureScreenDemand::Result evaluateAt(const TextureScreenDemand::View& view, const TexelDensity& texel, int32_t previous,
                                       uint32_t previousClass, int32_t previousPresent, Distance distance, Path path) {
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
  const double now = std::min(distance(view.hero),distance(view.camera));
  // the closest point of the paths is never farther than the points they start at
  const bool   ahead = view.lookAhead();
  const double d = ahead ? std::min({path(view.hero,view.heroAhead),path(view.camera,view.cameraAhead),now}) : now;
  const auto demand = TextureLevels::demand(d,texel.uvPerMetre,
                                            previous==TextureScreenDemand::WholeChain ? TextureLevels::NoExponent : previous,
                                            view.coarsest);
  if(demand.exponent==TextureLevels::NoExponent) {
    ret.exponent = TextureScreenDemand::WholeChain;
    ret.present  = TextureScreenDemand::WholeChain;
    return ret;
    }
  // The present exponent errs sharper as the other one does; the owner is due when either may change. Both distances
  // change by at most the odometer's step (ChunkResidency adds the paths' moves to it).
  double margin = demand.margin;
  ret.present = demand.exponent;
  if(ahead) {
    const auto present = TextureLevels::demand(now,texel.uvPerMetre,
                                               previousPresent==TextureScreenDemand::WholeChain ? TextureLevels::NoExponent
                                                                                                : previousPresent,
                                               view.coarsest);
    if(present.exponent!=TextureLevels::NoExponent) {
      ret.present = std::min(present.exponent,demand.exponent);
      margin = std::min(margin,present.margin);
      }
    }
  // due again when the exponent, or while the stages use it the distance class, may change
  double classMargin = 0;
  ret.distanceClass = TextureScreenDemand::distanceClass(d,previousClass,classMargin);
  ret.exponent  = demand.exponent;
  ret.margin    = view.classMargin ? std::max(std::min(margin,classMargin),TextureLevels::MinMargin) : margin;
  ret.expiry    = view.odometer+ret.margin;
  ret.scheduled = true;
  return ret;
  }
}

TextureScreenDemand::Result TextureScreenDemand::evaluate(const View& view, const TexelDensity& texel, int32_t previous,
                                                          uint32_t previousClass, int32_t previousPresent) {
  return evaluateAt(view,texel,previous,previousClass,previousPresent,
                    [&](const Tempest::Vec3& p) { return distance(p,texel); },
                    [&](const Tempest::Vec3& p, const Tempest::Vec3& a) { return TextureLookAhead::distance(p,a,texel.min,texel.max); });
  }

TextureScreenDemand::Result TextureScreenDemand::evaluate(const View& view, const TexelDensity& texel, const Sphere& bounds,
                                                          int32_t previous, uint32_t previousClass, int32_t previousPresent) {
  return evaluateAt(view,texel,previous,previousClass,previousPresent,
                    [&](const Tempest::Vec3& p) { return distance(p,bounds); },
                    [&](const Tempest::Vec3& p, const Tempest::Vec3& a) {
                      return TextureLookAhead::distance(p,a,bounds.centre,bounds.radius);
                      });
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
