#pragma once

#include <string>
#include <string_view>

namespace daedalus { class DaedalusScript; }

daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language, std::string* recordSha256 = nullptr);
daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language,
                                       std::string_view branch, std::u16string_view hostPath = {}, std::string* recordSha256 = nullptr);
