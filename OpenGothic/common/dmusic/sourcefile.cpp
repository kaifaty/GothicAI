#include "sourcefile.h"

#include "utils/fileutil.h"

#include <stdexcept>
#include <system_error>

Tempest::RFile Dx8::openSourceFile(const std::vector<std::u16string>& paths, const char16_t* file,
                                 std::u16string& openedPath) {
  for(const auto& path:paths) {
    try {
      auto filepath = FileUtil::nestedPath(path,{file},Tempest::Dir::FT_File);
      Tempest::RFile input(filepath);
      openedPath = std::move(filepath);
      return input;
      }
    catch(std::system_error&) {
      }
    }
  throw std::runtime_error("file not found");
  }
