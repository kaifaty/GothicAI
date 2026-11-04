#pragma once

namespace ZenKitLog {
enum class Level { Info, Warning };

void install(Level level);
void silenceForVideo();
}
