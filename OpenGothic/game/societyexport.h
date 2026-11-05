#pragma once

#include <string>

class World;

// The world of the society stand (docs/society-sim.md, «Интерфейсы → 1. Выгрузка мира», format v3): the
// waynet and its freepoints, every mobsi, every NPC with the daily routine in force. It reads the world and
// never changes it. `gothic-body-runner --world <zen> --society-export <path>` writes it right after the
// world's startup scripts (day 0, 08:00); gothic-society-export-contract checks it on the game's data.
namespace SocietyExport {

struct Result {
  std::string json;     // the file, records in id order: the same world gives the same bytes
  std::string summary;  // one line of counts for the log
  };

Result build(World& world);

}
