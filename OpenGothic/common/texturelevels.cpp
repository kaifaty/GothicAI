#include "texturelevels.h"

#include <Tempest/Pixmap>

#include <algorithm>
#include <cmath>

uint8_t TextureLevels::lowest(uint32_t width, uint32_t height, uint32_t mipCount) {
  uint32_t selected = 0;
  while(selected+1<mipCount && width/2>=MinSide && height/2>=MinSide) {
    ++selected;
    width  /= 2;
    height /= 2;
    }
  return uint8_t(std::min<uint32_t>(selected,255));
  }

uint8_t TextureLevels::level(uint32_t width, uint32_t height, float log2K, int32_t exponent, uint32_t reserve, uint8_t lowest) {
  if(exponent==NoExponent || width==0 || height==0)
    return 0;
  const double top  = 0.5*std::log2(double(width)*double(height));
  const double need = std::floor(top-double(log2K)-double(exponent));
  if(!std::isfinite(need))
    return 0;
  const double result = need-double(reserve);
  if(result<=0)
    return 0;
  return uint8_t(std::min(double(lowest),result));
  }

int32_t TextureLevels::coarsest(uint32_t reserve, float log2K) {
  const double e = std::floor(6.0-double(std::min<uint32_t>(reserve,64))-double(log2K));
  if(!std::isfinite(e))
    return std::numeric_limits<int32_t>::min();
  return int32_t(std::clamp(e,-1e6,1e6));
  }

float TextureLevels::viewScale(float height, float fovYDegrees) {
  const double half = double(fovYDegrees)*0.5*3.14159265358979323846/180.0;
  const double t    = std::tan(half);
  if(!(height>0) || !(t>0) || !std::isfinite(t))
    return 0;
  return float(std::log2(double(height)/(2.0*t)));
  }

TextureLevels::Demand TextureLevels::demand(double distance, float uvPerMetre, int32_t previous, int32_t coarsest) {
  Demand ret;
  if(!(uvPerMetre>0) || !std::isfinite(uvPerMetre) || !std::isfinite(distance))
    return ret;
  const double d = std::max(distance,double(MinDistance));
  const double x = 1.0/(d*double(uvPerMetre));
  if(!(x>0) || !std::isfinite(x))
    return ret;
  const double e = std::ceil(std::log2(x));
  if(e<double(std::numeric_limits<int32_t>::min()+1) || e>double(std::numeric_limits<int32_t>::max()-1))
    return ret;
  ret.exponent = int32_t(e);
  // exponent e holds while 1/(u*2^e) <= d < 2/(u*2^e); a sharper previous one up to (1+Hysteresis) times further
  const auto band = [&](int32_t exponent) { return 1.0/(double(uvPerMetre)*std::ldexp(1.0,exponent)); };
  if(previous!=NoExponent && previous>ret.exponent && previous-ret.exponent<64 &&
     d<2.0*band(previous)*(1.0+Hysteresis))
    ret.exponent = previous;
  if(ret.exponent<=coarsest) {
    // Any coarser changes no level: only coming closer than the band of `coarsest` does.
    ret.exponent = coarsest;
    ret.margin   = std::max(distance-band(coarsest),MinMargin);
    return ret;
    }
  const double lo = band(ret.exponent);
  const double hi = 2.0*lo*(1.0+Hysteresis);
  double margin = hi-distance;
  if(lo>double(MinDistance))
    margin = std::min(margin,distance-lo);
  ret.margin = std::max(margin,MinMargin);
  return ret;
  }

uint64_t TextureLevels::chainBytes(Tempest::TextureFormat format, uint32_t width, uint32_t height, uint32_t mipCount, uint32_t first) {
  const auto block = Tempest::Pixmap::blockSizeForFormat(format);
  uint64_t   sum   = 0;
  for(uint32_t i=first; i<std::max<uint32_t>(1,mipCount) && i<32; ++i) {
    const uint32_t w = std::max<uint32_t>(1,width>>i);
    const uint32_t h = std::max<uint32_t>(1,height>>i);
    const auto     c = Tempest::Pixmap::blockCount(format,w,h);
    sum += uint64_t(c.w)*uint64_t(c.h)*block;
    }
  return sum;
  }
