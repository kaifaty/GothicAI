#pragma once

#include "game/gametime.h"
#include "game/scripttypes.h"

#include <string>
#include <string_view>
#include <vector>

class WayPoint;

namespace NpcRoutineTable {
struct Row final {
  gtime            start;
  gtime            end;
  ScriptFn         callback;
  const WayPoint*  point = nullptr;
  std::string      fallbackName;

  std::string_view wayPointName() const;
  };

// The NPC resolves the point in its own world before appending the row.
void add(std::vector<Row>& routines, gtime start, gtime end, uint32_t callback,
         const WayPoint* point, std::string_view name);
}
