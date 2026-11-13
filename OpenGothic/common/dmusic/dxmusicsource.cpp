#include "dxmusicsource.h"

#include "directmusic.h"
#include "musicrecord.h"
#include "resources.h"

#include <Tempest/TextCodec>
#include <mutex>

Dx8::PatternList loadDxMusic(std::string_view name) {
  if(Resources::isHeadless())
    return Dx8::PatternList();

  static std::mutex sync;
  std::lock_guard<std::mutex> guard(sync);
  static const std::vector<std::u16string> musicPaths = {
    u"_work/Data/Music/newworld/",
    u"_work/Data/Music/AddonWorld/",
    u"_work/Data/Music/dungeon/",
    u"_work/Data/Music/menu_men/",
    u"_work/Data/Music/orchestra/",
    u"_work/Data/Music/"
    };
  static Dx8::DirectMusic music(Dx8::DirectMusic::Native{
    [](const char16_t* file) {
      return MusicData::decodeDx8Segment(Resources::readDxMusic(musicPaths,file,"segment"));
      },
    [](const char16_t* file) {
      return MusicData::decodeDx8Style(Resources::readDxMusic(musicPaths,file,"style"));
      },
    [](const char16_t* file) {
      return MusicData::decodeDx8Dls(Resources::readDxMusic(musicPaths,file,"dls"));
      }
    });
  auto u = Tempest::TextCodec::toUtf16(name);
  return music.load(u.c_str());
  }
