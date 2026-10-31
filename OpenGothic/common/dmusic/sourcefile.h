#pragma once

#include <Tempest/File>

#include <string>
#include <vector>

namespace Dx8 {
// The existing DirectMusic file search, shared with the source importer.
Tempest::RFile openSourceFile(const std::vector<std::u16string>& paths, const char16_t* file,
                             std::u16string& openedPath);
}
