#pragma once

#include <cstdint>

namespace zenkit { enum class LogLevel : std::uint8_t; }

namespace ZenKitLog {
enum class Level { Info, Warning };

void install(Level level);
void write(zenkit::LogLevel level, const char* component, const char* message);
void silenceForVideo();
}
