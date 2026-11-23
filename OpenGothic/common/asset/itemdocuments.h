#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace ItemDefinitions {
inline constexpr std::string_view DocumentFormat = "gothic-item-documents";
inline constexpr int32_t DocumentVersion = 1;

// The document setters supported by the static record, with their argument kinds after the handle.
inline std::string_view documentArguments(std::string_view function) {
  constexpr std::pair<std::string_view,std::string_view> signatures[]={
    {"DOC_SETPAGES","i"},{"DOC_SETPAGE","isi"},{"DOC_SETFONT","is"},{"DOC_SETMARGINS","iiiiii"},
    {"DOC_PRINTLINE","is"},{"DOC_PRINTLINES","is"},{"DOC_SETLEVEL","s"},{"DOC_SETLEVELCOORDS","iiii"},
    };
  for(const auto& [name,types]:signatures)
    if(name==function)
      return types;
  return {};
  }
}
