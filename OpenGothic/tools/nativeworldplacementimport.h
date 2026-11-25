#pragma once

#include "definitionjson.h"

namespace daedalus { class DaedalusScript; }

namespace NativeWorldPlacement {
rapidjson::Document prepare(const std::filesystem::path& base,std::string_view programSha,
                           const daedalus::DaedalusScript& script);
}
