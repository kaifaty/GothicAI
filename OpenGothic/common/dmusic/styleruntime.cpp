#include "style.h"

using namespace Dx8;

const Style::Part* Style::findPart(const GUID &guid) const {
  for(auto& i:parts)
    if(i.header.guidPartID==guid)
      return &i;
  return nullptr;
  }

uint32_t Pattern::timeLength(double tempo) const {
  const uint32_t PulsesPerQuarterNote  = uint32_t(600*(100.0/tempo));
  uint32_t mul = (header.timeSig.bBeatsPerMeasure*PulsesPerQuarterNote*4)/header.timeSig.bBeat;
  return header.wNbrMeasures * mul;
  }
