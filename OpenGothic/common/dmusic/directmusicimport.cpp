#include "directmusic.h"
#include "sourcefile.h"

using namespace Dx8;

DirectMusic::DirectMusic() {
  const auto read=[this](const char16_t* file) {
    std::u16string path;
    auto input=openSourceFile(this->path,file,path);
    std::vector<uint8_t> data(size_t(input.size()));
    input.read(data.data(),data.size());
    return data;
    };
  native.segment=[read](const char16_t* file) { auto data=read(file); Riff riff(data.data(),data.size()); return Segment(riff); };
  native.style=[read](const char16_t* file) { auto data=read(file); Riff riff(data.data(),data.size()); return Style(riff); };
  native.dls=[read](const char16_t* file) { auto data=read(file); Riff riff(data.data(),data.size()); return DlsCollection::parse(riff); };
  }

void DirectMusic::addPath(std::u16string p) {
  path.emplace_back(std::move(p));
  }
