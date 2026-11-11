#pragma once

#include <string>
#include <vector>

class World;
namespace NavSimPackage {
// navdemos dump-package REVISION PLACES TRAIN VALIDATION TEST NPC...
void write(const std::vector<std::string>& args, World& world, const std::string& output);
}
