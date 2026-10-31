#pragma once

#include <string>
#include <vector>

class World;

// Read-only physical qualification of safety fixtures. Truth never enters the
// navigation teacher; the separately reset episodes use ordinary start-v3.
namespace NavPerceptionSafety {
void command(const std::vector<std::string>& args, World& world,
             const std::string& outputDirectory);
}
