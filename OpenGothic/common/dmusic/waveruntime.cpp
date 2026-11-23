#include "wave.h"

using namespace Dx8;

void Wave::toFloatSamples(float* out) const {
  const int16_t* smp = reinterpret_cast<const int16_t*>(wavedata.data());
  for(size_t i=0;i<wavedata.size();i+=sizeof(int16_t), ++smp) {
    *out = (*smp)/32767.f;
    ++out;
    }
  }
