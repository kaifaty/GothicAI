#include "testbench_internal.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../game/societyexport.h"
#include "game/gamesession.h"
#include "world/world.h"

using namespace testbench_detail;

// Bench-only society commands over the LIVE world, after bench commands such as `npcscale` have
// changed it. `society export <name>` writes the stand's world (format v3) for the offline stand.
// `society start <name>` writes that export and engages the permanent Society pilot on it at the
// current game time, so a population probe can A/B the existing far-ownership machinery at scale.
// The export and the start happen within one command (no world step between them): the projection's
// fresh-world check requires startTime == now. The pilot itself is the existing flag-gated engine
// path; without this command the game is unchanged.

namespace {

std::string sanitizeName(std::string name) {
  for(char& c:name)
    if(!(std::isalnum(uint8_t(c)) || c=='-' || c=='_' || c=='.'))
      c='-';
  return name;
  }

// Writes the live world's export; returns the path, or empty on failure.
std::string writeExport(World& w, const std::string& name) {
  const auto result = SocietyExport::build(w);
  const auto path   = outDir+"/"+name;
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(result.json.data(),std::streamsize(result.json.size()));
  out.flush();
  const bool ok = bool(out);
  out.close();
  if(!ok) {
    say("society: cannot write "+path);
    return {};
    }
  say("society export: "+path+"; "+result.summary+"; "+std::to_string(result.json.size())+" bytes");
  event("society_export","\"path\":"+jsonStr(path)+",\"bytes\":"+std::to_string(result.json.size()));
  return path;
  }

}

namespace testbench_detail {

bool societyExportCommand(const std::vector<std::string>& a, World& w) {
  if(a.size()<2) {
    say("society: usage: society export <name> | society start <name>");
    return true;
    }
  const std::string name = sanitizeName(a.size()>2 ? a[2] : "world.json");
  if(a[1]=="export") {
    writeExport(w,name);
    return true;
    }
  if(a[1]=="start") {
    const auto path = writeExport(w,name);
    if(path.empty())
      return true;
    setenv("OPENGOTHIC_SOCIETY_WORLD",path.c_str(),1);
    setenv("OPENGOTHIC_SOCIETY_PILOT_GROUP","32",1);
    setenv("OPENGOTHIC_SOCIETY_PILOT_WIDE","1",1);
    setenv("OPENGOTHIC_SOCIETY_PILOT_EARLY","1",1);
    w.startSocietyProjection(GameSession::clockRate());
    const bool active = w.persistentSocietyEnabled();
    say(std::string("society start: ")+(active ? "permanent pilot active":"projection inactive (world mismatch)"));
    event("society_start",std::string("\"active\":")+(active ? "true":"false"));
    return true;
    }
  say("society: usage: society export <name> | society start <name>");
  return true;
  }

}
