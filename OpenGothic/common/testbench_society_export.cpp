#include "testbench_internal.h"

#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "../game/societyexport.h"
#include "world/world.h"

// Bench-only: `society export <name>` writes the society stand's world (format v3) of the LIVE world,
// after bench commands such as `npcscale` have changed it. The runner's --society-export writes the
// same format, but only right after the startup scripts; a population probe needs the export of the
// spawned world, and so does a Society ownership A/B at scale (the pilot matches the NPC list).
// Reads the world, never changes it.

namespace testbench_detail {

bool societyExportCommand(const std::vector<std::string>& a, World& w) {
  if(a.size()<2 || a[1]!="export") {
    say("society: usage: society export <name>");
    return true;
    }
  std::string name = a.size()>2 ? a[2] : "world.json";
  for(char& c:name)
    if(!(std::isalnum(uint8_t(c)) || c=='-' || c=='_' || c=='.'))
      c='-';
  const auto result = SocietyExport::build(w);
  const auto path   = outDir+"/"+name;
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(result.json.data(),std::streamsize(result.json.size()));
  out.flush();
  const bool ok = bool(out);
  out.close();
  if(!ok) {
    say("society export: cannot write "+path);
    return true;
    }
  say("society export: "+path+"; "+result.summary+"; "+std::to_string(result.json.size())+" bytes");
  event("society_export","\"path\":"+jsonStr(path)+",\"bytes\":"+std::to_string(result.json.size()));
  return true;
  }

}
