#include "dlscollection.h"

#include <Tempest/Log>
#include <Tempest/MemWriter>
#include <stdexcept>
#include <fstream>

#include "soundfont.h"

using namespace Dx8;
using namespace Tempest;

DlsCollection::Region::Region(Riff &input) {
  if(!input.is("LIST"))
    throw std::runtime_error("not a riff");
  input.readListId("rgn ");
  input.read([this](Riff& c){
    implRead(c);
    });
  }

void DlsCollection::Region::implRead(Riff &input) {
  if(input.is("rgnh")){
    input.read(&head,sizeof(head));
    }
  else if(input.is("LIST") && input.isListId("lart")){
    // articulators
    }
  else if(input.is("wlnk")){
    input.read(&wlink,sizeof(wlink));
    }
  else if(input.is("wsmp")){
    input.read(&waveSample,sizeof(waveSample));
    loop.resize(waveSample.cSampleLoops);
    for(auto& i:loop){
      input.read(&i,sizeof(i));
      input.skip(i.cbSize-sizeof(i));
      }
    }
  }


DlsCollection::Articulator::Articulator(Riff &input) {
  if(!input.is("art1"))
    throw std::runtime_error("not a riff");

  uint32_t cbSize=0;
  uint32_t size=0;

  input.read(&cbSize,sizeof(cbSize));
  input.read(&size,sizeof(size));
  connectionBlocks.resize(size);

  for(auto& i:connectionBlocks) {
    input.read(&i,sizeof(i));
    }
  }

DlsCollection::Instrument::Instrument(Riff &input) {
  if(!input.is("LIST"))
    throw std::runtime_error("not a list");
  input.readListId("ins ");
  input.read([this](Riff& c){
    implRead(c);
    });
  }

void DlsCollection::Instrument::implRead(Riff &input) {
  if(input.is("LIST")){
    if(input.isListId("lrgn")){
      input.read([this](Riff& c){
        regions.emplace_back(c);
        });
      }
    else if(input.isListId("lart")){
      input.read([this](Riff& c){
        articulators.emplace_back(c);
        });
      }
    else if(input.isListId("INFO")){
      info = Info(input);
      }
    }
  else if(input.is("insh")){
    input.read(&header,sizeof(header));
    }
  }

DlsCollection::DlsCollection(Riff& input)
  :DlsCollection(parse(input)) {
  }

DlsCollection::Parsed DlsCollection::parse(Riff& input) {
  Parsed ret;
  if(!input.is("RIFF"))
    throw std::runtime_error("not a riff");
  input.readListId("DLS ");
  input.read([&ret](Riff& c){
    implRead(ret,c);
    });
  return ret;
  }

void DlsCollection::implRead(Parsed& out, Riff &input) {
  if(input.is("vers"))
    input.read(&out.version,sizeof(out.version));
  else if(input.is("dlid"))
    input.read(&out.dlid,sizeof(out.dlid));
  else if(input.is("LIST")){
    if(input.isListId("wvpl")){
      input.read([&out](Riff& chunk){
        Wave wx(chunk);
        out.wave.emplace_back(std::move(wx));
        });
      }
    else if(input.isListId("lins")){
      input.read([&out](Riff& chunk){
        out.instrument.emplace_back(chunk);
        });
      }
    }
  }
