#include "texturelookahead.h"

#include <algorithm>
#include <array>
#include <cmath>

double TextureLookAhead::horizon(double latencyP90Ms, uint64_t samples) {
  if(samples<MinSamples || !std::isfinite(latencyP90Ms) || latencyP90Ms<0)
    return DefaultHorizon;
  return std::clamp(HorizonFactor*latencyP90Ms*0.001,0.0,MaxHorizon);
  }

void TextureLookAhead::Velocity::add(const Tempest::Vec3& position, double seconds) {
  if(!history.empty() && seconds<history.back().first)
    history.clear();
  history.emplace_back(seconds,position);
  // keep one position at least Window old, so the velocity spans the whole window once it is full
  while(history.size()>2 && seconds-history[1].first>=Window)
    history.pop_front();
  }

Tempest::Vec3 TextureLookAhead::Velocity::velocity() const {
  if(history.size()<2)
    return {};
  const auto& [t0,p0] = history.front();
  const auto& [t1,p1] = history.back();
  const double span = t1-t0;
  if(!(span>=0.001))
    return {};
  return Tempest::Vec3(float((double(p1.x)-p0.x)/span),float((double(p1.y)-p0.y)/span),float((double(p1.z)-p0.z)/span));
  }

double TextureLookAhead::distance(const Tempest::Vec3& from, const Tempest::Vec3& ahead, const Tempest::Vec3& min,
                                  const Tempest::Vec3& max) {
  const double p[3] = {from.x,from.y,from.z}, a[3] = {ahead.x,ahead.y,ahead.z};
  const double lo[3] = {min.x,min.y,min.z},   hi[3] = {max.x,max.y,max.z};
  // The squared distance from p+t*a to the box is convex and quadratic between the t where an axis enters or leaves
  // its slab: on each such interval take the vertex of its quadratic, clamped to the interval.
  std::array<double,8> cuts{};
  size_t n = 0;
  cuts[n++] = 0;
  cuts[n++] = 1;
  for(int i=0;i<3;++i)
    if(a[i]!=0)
      for(const double bound:{lo[i],hi[i]}) {
        const double t = (bound-p[i])/a[i];
        if(t>0 && t<1)
          cuts[n++] = t;
        }
  std::sort(cuts.begin(),cuts.begin()+ptrdiff_t(n));
  const auto at = [&](double t) {
    double sum = 0;
    for(int i=0;i<3;++i) {
      const double x = p[i]+t*a[i];
      const double d = std::max({lo[i]-x,0.0,x-hi[i]});
      sum += d*d;
      }
    return sum;
    };
  double best = std::min(at(0),at(1));
  for(size_t k=0;k+1<n;++k) {
    const double t0 = cuts[k], t1 = cuts[k+1];
    if(!(t1>t0))
      continue;
    const double mid = 0.5*(t0+t1);
    double qa = 0, qb = 0;
    for(int i=0;i<3;++i) {
      const double x = p[i]+mid*a[i];
      // outside the slab on this interval: (p + t*a - bound)^2
      const double bound = x<lo[i] ? lo[i] : x>hi[i] ? hi[i] : std::nan("");
      if(std::isnan(bound))
        continue;
      qa += a[i]*a[i];
      qb += 2*a[i]*(p[i]-bound);
      }
    if(qa>0)
      best = std::min(best,at(std::clamp(-qb/(2*qa),t0,t1)));
    }
  return std::sqrt(best)*0.01;
  }

double TextureLookAhead::distance(const Tempest::Vec3& from, const Tempest::Vec3& ahead, const Tempest::Vec3& centre,
                                  float radius) {
  const double px = double(centre.x)-from.x, py = double(centre.y)-from.y, pz = double(centre.z)-from.z;
  const double ax = ahead.x, ay = ahead.y, az = ahead.z;
  const double len = ax*ax+ay*ay+az*az;
  const double t = len>0 ? std::clamp((px*ax+py*ay+pz*az)/len,0.0,1.0) : 0.0;
  const double dx = px-t*ax, dy = py-t*ay, dz = pz-t*az;
  return std::max(std::sqrt(dx*dx+dy*dy+dz*dz)-double(radius),0.0)*0.01;
  }
