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

void earLean(uint64_t h, int32_t x, int32_t z, float& angle, float& lean) {
  // the ears of a place hang to one side, as the wind has combed them: the side turns slowly over spots of 15 m
  const float place = float(valueNoise(x,z,1500*Unit,0xE7u))/65280.f;
  angle = 0.9f + (place-0.5f)*2.4f + (float((h >> 43) & 0xFFu)/255.f-0.5f)*0.9f;
  lean  = 8.f + 10.f*float((h >> 51) & 0x3Fu)/63.f;
  }

}
