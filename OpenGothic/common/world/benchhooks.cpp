#include "benchhooks.h"

std::function<bool()> Bench::aiBusy;
std::function<bool(std::string_view key, std::string_view stage)> Bench::aiReached;
std::function<bool(World&,std::string_view,float)> Bench::stuckBegin;
std::function<bool(World&)> Bench::stuckEnd;
std::function<std::string(World&,std::string_view npc)> Bench::npcView;
std::function<void(std::string_view menu)> Bench::openMenu;
