#pragma once

#include <cstdint>
#include <string>
#include <vector>
class World;
namespace NpcDayMeasurement {
bool start(World& world, const std::string& directory, uint32_t seed, bool isolate,
           const std::vector<std::string>& selected, std::string& error);
void tick(World& world);
void stop(World& world);
}
