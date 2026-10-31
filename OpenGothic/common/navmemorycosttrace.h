#pragma once

#include <game/navigationmemory.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace NavMemoryCost {
inline constexpr std::array<char,8> Magic={'N','A','V','C','S','T','0','1'};
enum : uint8_t { Update=1, View=2 };

// Local benchmark captures are replayed on the same machine and build ABI.
template<class T> void write(std::ostream& output,const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  output.write(reinterpret_cast<const char*>(&value),sizeof(value));
  }
template<class T> void read(std::istream& input,T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  input.read(reinterpret_cast<char*>(&value),sizeof(value));
  if(!input)throw std::runtime_error("truncated navigation memory cost capture");
  }
}

#ifndef NAV_MEMORY_COST_REPLAY_ONLY
// Stand-only capture: initial personal familiar knowledge, every real body tick,
// and the actual readout schedule. The learner's input and sensor are unchanged.
class NavMemoryCostTrace final {
  public:
    static bool enabled(const std::filesystem::path& output) {
      const char* setting=std::getenv("OPENGOTHIC_NAV_MEMORY_TRACE");
      if(setting==nullptr)return false;
      std::string_view selected(setting);
      if(selected=="0")return false;
      const auto name=output.filename().string();
      while(!selected.empty()) {
        const auto comma=selected.find(',');
        if(selected.substr(0,comma)==name)return true;
        if(comma==std::string_view::npos)break;
        selected.remove_prefix(comma+1);
        }
      return false;
      }

    NavMemoryCostTrace(const std::filesystem::path& path,const NavigationMemory& memory)
      :output(path,std::ios::binary) {
      if(!output)throw std::runtime_error("cannot open navigation memory cost capture");
      output.write(NavMemoryCost::Magic.data(),NavMemoryCost::Magic.size());
      const uint32_t poseBytes=sizeof(NavigationMemory::Pose),rayBytes=sizeof(NavigationEye::Ray);
      const uint32_t tileBytes=sizeof(NavigationMemory::FamiliarTile);
      NavMemoryCost::write(output,poseBytes);NavMemoryCost::write(output,rayBytes);NavMemoryCost::write(output,tileBytes);
      const bool familiar=memory.familiarMapEnabled();
      const uint64_t tiles=familiar?memory.familiarMap->tiles.size():0;
      const uint64_t words=familiar?memory.familiarMask.size():0;
      NavMemoryCost::write(output,tiles);NavMemoryCost::write(output,words);
      if(familiar) {
        for(const auto& tile:memory.familiarMap->tiles)NavMemoryCost::write(output,tile);
        for(const auto word:memory.familiarMask)NavMemoryCost::write(output,word);
        }
      }

    void update(const NavigationMemory::Pose& pose,const std::vector<NavigationEye::Ray>& rays) {
      const uint8_t event=NavMemoryCost::Update;
      NavMemoryCost::write(output,event);NavMemoryCost::write(output,pose);
      const uint32_t count=uint32_t(rays.size());
      uint32_t changed=0;
      const auto differs=[&](size_t i) {
        return i>=previous.size() || std::memcmp(&previous[i],&rays[i],sizeof(rays[i]))!=0;
        };
      for(size_t i=0;i<rays.size();++i)changed+=differs(i);
      NavMemoryCost::write(output,count);NavMemoryCost::write(output,changed);
      for(uint32_t i=0;i<count;++i)if(differs(i)) {
        NavMemoryCost::write(output,i);NavMemoryCost::write(output,rays[i]);
        }
      previous=rays;
      if(!output)throw std::runtime_error("navigation memory cost capture write failed");
      }

    void view() {
      const uint8_t event=NavMemoryCost::View;
      NavMemoryCost::write(output,event);
      if(!output)throw std::runtime_error("navigation memory cost capture write failed");
      }

  private:
    std::ofstream output;
    std::vector<NavigationEye::Ray> previous;
  };
#endif
