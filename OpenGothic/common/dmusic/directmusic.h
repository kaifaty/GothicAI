#pragma once

#include "dlscollection.h"
#include "patternlist.h"
#include "segment.h"
#include "style.h"

#include <Tempest/File>
#include <functional>
#include <vector>

namespace Dx8 {

/**
 * http://doc.51windows.net/Directx9_SDK/htm/directmusicfilestructures.htm
 */
class DirectMusic final {
  public:
    struct Native {
      std::function<Segment(const char16_t*)> segment;
      std::function<Style(const char16_t*)> style;
      std::function<DlsCollection::Parsed(const char16_t*)> dls;
      };

    DirectMusic();
    explicit DirectMusic(Native source);

    using StyleList = std::vector<std::pair<std::u16string,Style>>;
    using DlsList   = std::vector<std::unique_ptr<std::pair<std::u16string,DlsCollection>>>;

    PatternList          load(const Segment& s);
    PatternList          load(const char16_t* fsgt);

    void addPath(std::u16string path);

    const Style&         style        (const Reference &id);
    const DlsCollection& dlsCollection(const Reference &id);
    const DlsCollection& dlsCollection(const std::u16string &file);

    const StyleList&     stlList() const { return styles; }
    const DlsList&       dlsCollection() { return dls;    }

  private:
    Native                      native;
    StyleList                   styles;
    DlsList                     dls;
    std::vector<std::u16string> path;
  };

}
