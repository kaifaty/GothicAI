#include "testbench_internal.h"

#include "graphics/grass/grasscage.h"

namespace testbench_detail {

bool grassCageCommand(const std::vector<std::string>& args) {
  const auto steps = args.size()==3 ? GrassCage::parseSteps(args[2]) : -1;
  if(steps<0) {
    arenaAbort("grass cage expects 0, 1 or 2");
    return true;
    }
  GrassCage::setSteps(steps);
  event("grass_cage", "\"steps\":"+std::to_string(steps));
  return true;
  }

}
