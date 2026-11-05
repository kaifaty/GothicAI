#include "journal.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Society {
namespace {

template<class T>
void put(std::string& out, T v) {
  // Little-endian whatever the machine.
  using U = std::make_unsigned_t<T>;
  const U u = U(v);
  for(size_t i=0; i<sizeof(T); ++i)
    out.push_back(char(uint8_t(u>>(8*i))));
  }

uint32_t time32(Time t) {
  if(t<0 || t>Time(std::numeric_limits<uint32_t>::max()))
    throw std::out_of_range("society journal: a time does not fit the record's u32 field");
  return uint32_t(t);
  }

// As the engine's journal cleans text for its CSV (common/npcdayjournal.cpp).
void clean(std::string& out, std::string_view s) {
  if(s.empty()) {
    out += '-';
    return;
    }
  for(char c:s)
    out += (c==',' || c=='\n' || c=='\r' || c=='"') ? '_' : c;
  }

void minutes(std::string& out, Time seconds) {
  char buf[32];
  const int n = std::snprintf(buf,sizeof(buf),"%.2f",double(seconds)/60.0);
  out.append(buf,size_t(n));
  }

}

namespace Journal {

std::string header(const World& w, uint64_t seed) {
  const size_t needs = needCount(w);
  std::string out(Magic,sizeof(Magic));
  put(out,needs>0 ? NeedsVersion : Version);
  put(out,RecordBytes);
  put(out,w.exportHash);
  put(out,seed);
  put(out,int64_t(w.startTime()));
  put(out,uint32_t(w.npcs.size()));
  put(out,uint32_t(w.points.size()));
  put(out,uint32_t(w.stateNames.size()));
  for(const auto& name:w.stateNames) {
    put(out,uint16_t(name.size()));
    out += name;
    }
  if(needs==0)
    return out;
  put(out,w.config.hash);
  put(out,uint32_t(needs));
  for(const auto& id:w.config.needs.id) {
    put(out,uint16_t(id.size()));
    out += id;
    }
  return out;
  }

void append(std::string& out, std::span<const Event> events) {
  out.reserve(out.size()+events.size()*RecordBytes);
  for(const auto& e:events) {
    put(out,time32(e.time));
    put(out,e.npc);
    put(out,uint8_t(e.kind));
    put(out,e.reason);
    put(out,e.entry);
    put(out,e.a);
    put(out,e.b);
    put(out,e.kind==EventKind::TravelStarted ? time32(e.c) : uint32_t(e.c));
    }
  }

}

namespace Marks {

std::string header(const World& w) {
  std::string out = "t_min,key,routine,state,moving,point,to,arrive_min,x,y,z";
  for(const auto& id:w.config.needs.id)
    out += ",need_"+id;
  return out+"\n";
  }

void append(std::string& out, Sim& sim) {
  const auto& w = *sim.world;
  const auto& s = sim.s;
  const Time  start = w.startTime();
  const size_t needs = s.needSince.empty() ? 0 : needCount(w);
  char t[32];
  const int tn = std::snprintf(t,sizeof(t),"%lld,",(long long)((s.now-start)/60));
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    out.append(t,size_t(tn));
    out += w.npcs.key[i];
    out += ',';
    if(const auto e = s.entry[i]; e!=NoEntry) {
      const auto k = w.entryIndex(i,e);
      char buf[48];
      const int n = std::snprintf(buf,sizeof(buf),"%u %02u:%02u-%02u:%02u ",unsigned(e),
                                  unsigned(w.routines.start[k]/60),unsigned(w.routines.start[k]%60),
                                  unsigned(w.routines.end[k]/60),unsigned(w.routines.end[k]%60));
      out.append(buf,size_t(n));
      out += w.stateNames[w.routines.state[k]];
      out += ' ';
      clean(out,w.routines.pointName[k]);
      } else {
      out += '-';
      }
    out += ',';
    if(s.state[i]!=NoState)
      out += w.stateNames[s.state[i]]; else
      out += '-';
    const bool moving = (s.flags[i] & Moving)!=0;
    out += moving ? ",1," : ",0,";
    clean(out,w.points.name[s.place[i]]);
    out += ',';
    std::array<float,3> at = w.points.pos[s.place[i]];
    if(moving) {
      clean(out,w.points.name[s.dest[i]]);
      out += ',';
      minutes(out,s.arrive[i]-start);
      const Time span = s.arrive[i]-s.depart[i];
      at = sim.paths.along(s.place[i],s.dest[i],span>0 ? double(s.now-s.depart[i])/double(span) : 1.0);
      } else {
      out += "-,-";
      }
    char pos[96];
    const int pn = std::snprintf(pos,sizeof(pos),",%.1f,%.1f,%.1f",double(at[0]),double(at[1]),double(at[2]));
    out.append(pos,size_t(pn));
    for(size_t k=0; k<needs; ++k) {
      if(!w.npcs.person[i]) {
        out += ",-";
        continue;
        }
      char level[32];
      const int ln = std::snprintf(level,sizeof(level),",%.4f",needLevel(sim,i,k));
      out.append(level,size_t(ln));
      }
    out += '\n';
    }
  }

}
}
