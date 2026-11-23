#pragma once

#include <string>
class World;
namespace NpcDayJournal { struct Options; }
namespace NpcDayMeasurement {
bool start(World& world, const NpcDayJournal::Options& options, std::string& error);
void tick(World& world);
void stop(World& world);
}
