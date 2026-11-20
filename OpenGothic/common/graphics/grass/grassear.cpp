#include "grassear.h"

#include "graphics/grass/grassplace.h"

namespace Grass {

Tuft evenTuft(const Tuft& t) {
  Tuft r = t;
  r.densityQ8 = 256;
  r.pullQ8    = 0;
  r.sward     = false;
  r.clump     = false;
  return r;
  }

float earHeight(uint64_t h, int32_t x, int32_t z) {
  const float own  = 0.9f + 0.2f*float((h >> 35) & 0xFFu)/255.f;
  const float spot = float(valueNoise(x,z,800*Unit,0xE1u))/65280.f;
  return own*(1.f - 0.2f*spot);
  }

void earLean(uint64_t h, float& angle, float& lean) {
  angle = float((h >> 43) & 0xFFu)*(6.2831853f/256.f);
  lean  = 8.f + 10.f*float((h >> 51) & 0x3Fu)/63.f;
  }

}
