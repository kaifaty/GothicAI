#pragma once

#include <string_view>

namespace daedalus { class DaedalusScript; }

daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language);
daedalus::DaedalusScript loadBaseProgram(std::string_view request, int language,
                                       std::string_view branch, std::u16string_view hostPath = {});
