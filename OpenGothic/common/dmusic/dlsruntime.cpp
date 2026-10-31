#include "dlscollection.h"
#include "soundfont.h"
#include <Tempest/Log>

using namespace Dx8;
using namespace Tempest;

DlsCollection::DlsCollection(Parsed&& parsed)
  :version(parsed.version), dlid(parsed.dlid), instrument(std::move(parsed.instrument)), wave(std::move(parsed.wave)) {
  shData = SoundFont::shared(*this,wave);
  wave.clear();
  }

SoundFont DlsCollection::toSoundfont(uint32_t dwPatch) const {
  return SoundFont(shData,dwPatch);
  }

void DlsCollection::dbgDump() const {
  Log::i("__DLS__");

  for(auto& i:instrument){
    for(auto& r:i.regions) {
      if(r.wlink.ulTableIndex<wave.size()) {
        Log::i(" ",r.head.RangeKey.usLow,"-",r.head.RangeKey.usHigh," ",r.wlink.ulTableIndex);//," ",wave[r.wlink.ulTableIndex].info.inam);
        } else {
        Log::i(" ",r.wlink.ulTableIndex);
        }
      }
    }
  }

const Wave* DlsCollection::findWave(uint8_t note) const {
  for(auto& i:instrument){
    for(auto& r:i.regions) {
      if(r.head.RangeKey.usLow<=note && note<=r.head.RangeKey.usHigh) {
        if(r.wlink.ulTableIndex<wave.size())
          return &wave[r.wlink.ulTableIndex];
        }
      }
    }
  return nullptr;
  }
