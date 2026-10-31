#include "directmusic.h"

using namespace Dx8;

DirectMusic::DirectMusic(Native source) : native(std::move(source)) {
  }

PatternList DirectMusic::load(const Segment &s) {
  return PatternList(s,*this);
  }

PatternList DirectMusic::load(const char16_t *fsgt) {
  return load(native.segment(fsgt));
  }

const Style &DirectMusic::style(const Reference &id) {
  for(auto& i:styles){
    if(i.first==id.file)
      return i.second;
    }

  styles.emplace_back(id.file,native.style(id.file.c_str()));
  return styles.back().second;
  }

const DlsCollection &DirectMusic::dlsCollection(const Reference &id) {
  return dlsCollection(id.file);
  }

const DlsCollection &DirectMusic::dlsCollection(const std::u16string &file) {
  for(auto& i:dls){
    if(i->first==file)
      return i->second;
    }
  DlsCollection value(native.dls(file.c_str()));
  dls.emplace_back(new std::pair<std::u16string,DlsCollection>(file,std::move(value)));
  return dls.back()->second;
  }
