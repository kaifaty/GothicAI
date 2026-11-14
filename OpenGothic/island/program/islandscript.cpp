#include "islandscript.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace island {

ScriptTrack::ScriptTrack(const std::filesystem::path& file) {
  std::ifstream input(file);
  if(!input)
    throw std::runtime_error("cannot read track: "+file.string());
  bool placed = false;
  std::set<std::string> names;
  std::string line;
  size_t number = 0;
  while(std::getline(input, line)) {
    ++number;
    line = line.substr(0, line.find('#'));
    std::istringstream in(line);
    std::string op, extra;
    if(!(in>>op))
      continue;
    auto fail = [&] { throw std::runtime_error(file.string()+":"+std::to_string(number)+": invalid "+op); };
    if(op=="start") {
      if(placed || !(in>>feet.x>>feet.y>>feet.z>>angle) || !std::isfinite(feet.x) || !std::isfinite(feet.y) ||
         !std::isfinite(feet.z) || !std::isfinite(angle))
        fail();
      feet *= 100.f;
      placed = true;
      }
    else if(op=="move") {
      Move m{};
      int run = 0;
      if(!(in>>m.time>>m.forward>>m.strafe>>m.turn>>m.pitch>>run) ||
         !std::isfinite(m.forward) || !std::isfinite(m.strafe) || !std::isfinite(m.turn) || !std::isfinite(m.pitch) ||
         std::abs(m.forward)>1 || std::abs(m.strafe)>1 || (run!=0 && run!=1) ||
         (!moves.empty() && m.time<=moves.back().time))
        fail();
      m.run = run!=0;
      moves.push_back(m);
      }
    else if(op=="jump") {
      uint64_t at = 0;
      if(!(in>>at) || (!jumps.empty() && at<=jumps.back()))
        fail();
      jumps.push_back(at);
      }
    else if(op=="shot") {
      Shot s{};
      if(!(in>>s.time>>s.name) || s.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos ||
         !names.insert(s.name).second || (!pictures.empty() && s.time<=pictures.back().time))
        fail();
      pictures.push_back(s);
      }
    else if(op=="end") {
      if(length!=0 || !(in>>length) || length==0)
        fail();
      }
    else {
      fail();
      }
    if(in>>extra)
      fail();
    }
  if(!placed || length==0 || moves.empty() || moves.front().time!=0 || moves.back().time>=length ||
     (!jumps.empty() && jumps.back()>=length) || (!pictures.empty() && pictures.back().time>=length))
    throw std::runtime_error("track needs start, move at 0 and end after all commands: "+file.string());
  }

Control ScriptTrack::next(uint64_t time, uint64_t dt) {
  Control c;
  if(finished(time))
    return c;
  const Move* active = &moves.front();
  for(const auto& m:moves) {
    if(m.time>time)
      break;
    active = &m;
    }
  c.forward = active->forward;
  c.strafe  = active->strafe;
  c.run     = active->run;
  c.turn    = active->turn*float(dt)*0.001f;
  c.pitch   = active->pitch*float(dt)*0.001f;
  for(auto at:jumps)
    c.jump = c.jump || (time<=at && at<time+dt);
  return c;
  }

}
