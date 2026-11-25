#include "core.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Society {
namespace {

constexpr std::string_view Magic = "GSOCCKPT";
constexpr uint32_t Version = 1;
constexpr size_t QueueRecordBytes = sizeof(Time)+sizeof(uint64_t)+sizeof(uint32_t)+sizeof(uint8_t);
constexpr Time MaxTime = std::numeric_limits<Time>::max()/2;
constexpr Time Never = std::numeric_limits<Time>::max();

void require(bool valid, const char* reason) {
  if(!valid)
    throw std::invalid_argument(std::string("society checkpoint: ")+reason);
  }

struct Writer {
  std::string bytes;

  template<class T>
  void scalar(T value) {
    uint64_t number = 0;
    if constexpr(std::is_same_v<T,double>)
      number = std::bit_cast<uint64_t>(value);
    else
      number = std::make_unsigned_t<T>(value);
    for(size_t i=0; i<sizeof(T); ++i)
      bytes.push_back(char(uint8_t(number>>(8*i))));
    }

  template<class T>
  void array(const std::vector<T>& data, size_t count) {
    require(data.size()==count,"state size does not match the world");
    for(T value:data)
      scalar(value);
    }

  void queue(const std::vector<Pending>& data, uint64_t count) {
    require(data.size()==count,"queue size does not match the schedule counters");
    for(const auto& p:data) {
      scalar(p.time);
      scalar(p.seq);
      scalar(p.npc);
      scalar(uint8_t(p.kind));
      }
    }
  };

struct Reader {
  std::string_view bytes;

  template<class T>
  void scalar(T& value) {
    require(bytes.size()>=sizeof(T),"truncated field");
    uint64_t number = 0;
    for(size_t i=0; i<sizeof(T); ++i)
      number |= uint64_t(uint8_t(bytes[i]))<<(8*i);
    if constexpr(std::is_same_v<T,double>)
      value = std::bit_cast<double>(number);
    else
      value = std::bit_cast<T>(std::make_unsigned_t<T>(number));
    bytes.remove_prefix(sizeof(T));
    }

  template<class T>
  void array(std::vector<T>& data, size_t count) {
    require(count<=bytes.size()/sizeof(T),"truncated state array");
    data.resize(count);
    for(auto& value:data)
      scalar(value);
    }

  void queue(std::vector<Pending>& data, uint64_t count) {
    require(count<=bytes.size()/QueueRecordBytes,"truncated event queue");
    data.resize(size_t(count));
    for(auto& p:data) {
      uint8_t kind = 0;
      scalar(p.time);
      scalar(p.seq);
      scalar(p.npc);
      scalar(kind);
      require(kind<=Pending::Recheck,"unknown event kind");
      p.kind = Pending::Kind(kind);
      }
    }
  };

template<class Codec, class S>
void transfer(Codec& io, S& s, const World& w, bool decisions) {
  const size_t npcs = w.npcs.size();
  const size_t needing = needCount(w)!=0 ? npcs : 0;
  const size_t needs = npcs*needCount(w);
  const size_t choosing = decisions ? npcs : 0;
  io.scalar(s.now);
  io.scalar(s.seq);
  io.scalar(s.processed);
  io.scalar(s.emitted);
  io.scalar(s.decisions);
  for(auto& word:s.rng.s)
    io.scalar(word);
  require(s.processed<=s.seq && s.seq<=uint64_t(MaxTime),"invalid schedule counters");
  io.array(s.place,npcs);
  io.array(s.dest,npcs);
  io.array(s.depart,npcs);
  io.array(s.arrive,npcs);
  io.array(s.bodySeq,npcs);
  io.array(s.entry,npcs);
  io.array(s.stateEntry,npcs);
  io.array(s.stateEnd,npcs);
  io.array(s.state,npcs);
  io.array(s.stateStart,npcs);
  io.array(s.flags,npcs);
  io.array(s.needLevel,needs);
  io.array(s.needRate,needs);
  io.array(s.needDecay,needs);
  io.array(s.needZone,needs);
  io.array(s.needSince,needing);
  io.array(s.needSeq,needing);
  io.array(s.needAt,needing);
  io.array(s.mode,choosing);
  io.array(s.want,choosing);
  io.array(s.deferred,choosing);
  io.array(s.actType,choosing);
  io.array(s.actKind,choosing);
  io.array(s.actPoint,choosing);
  io.array(s.actObject,choosing);
  io.array(s.actBegin,choosing);
  io.array(s.actMin,choosing);
  io.array(s.actEnd,choosing);
  io.array(s.actFinish,choosing);
  io.array(s.actSeq,choosing);
  io.array(s.recheckSeq,choosing);
  io.array(s.objectUse,decisions ? w.objects.size() : 0);
  io.array(s.pointUse,decisions ? w.points.size() : 0);
  io.array(s.routineTarget,choosing);
  io.array(s.routineKind,choosing);
  io.array(s.routineWait,choosing);
  io.array(s.forecastFor,choosing);
  io.queue(s.queue,s.seq-s.processed);
  }

bool validTime(Time t) {
  return t>=0 && t<=MaxTime;
  }

void validateQueue(const State& s, const World& w, bool decisions) {
  enum : uint8_t { Body = 1, Need = 2, Activity = 4, Recheck = 8 };
  std::vector<uint8_t> active(w.npcs.size());
  std::vector<uint64_t> serial;
  serial.reserve(s.queue.size());
  for(const auto& p:s.queue) {
    require(p.npc<w.npcs.size() && validTime(p.time) && p.time>=s.now && p.seq<s.seq,"invalid scheduled event");
    serial.push_back(p.seq);
    const auto i = p.npc;
    const auto key = p.seq+1;
    if(p.kind==Pending::Arrival || p.kind==Pending::Settle) {
      if(s.bodySeq[i]==key) {
        const bool moving = (s.flags[i] & Moving)!=0;
        require((p.kind==Pending::Arrival)==moving && (!moving || s.arrive[i]==p.time),"invalid body event");
        active[i] |= Body;
        }
      }
    else if(p.kind==Pending::Need) {
      require(!s.needSeq.empty() && w.npcNeeds[i]!=0,"unexpected need event");
      if(s.needSeq[i]==key) {
        require(s.needAt[i]==p.time,"invalid need event time");
        active[i] |= Need;
        }
      }
    else if(p.kind==Pending::Activity || p.kind==Pending::Recheck) {
      require(decisions && decides(w,i),"unexpected decision event");
      if(p.kind==Pending::Activity && s.actSeq[i]==key)
        active[i] |= Activity;
      if(p.kind==Pending::Recheck && s.recheckSeq[i]==key)
        active[i] |= Recheck;
      }
    else if(p.kind==Pending::RoutineChange)
      require(w.entryCount(i)!=0,"unexpected routine event");
    }
  std::sort(serial.begin(),serial.end());
  require(std::adjacent_find(serial.begin(),serial.end())==serial.end(),"duplicate event sequence");
  require(std::is_heap(s.queue.begin(),s.queue.end(),detail::later),"invalid event heap");
  for(size_t i=0; i<active.size(); ++i) {
    require(bool(active[i] & Body)==(s.bodySeq[i]!=0),"missing body event");
    if(!s.needSeq.empty())
      require(bool(active[i] & Need)==(s.needSeq[i]!=0),"missing need event");
    if(decisions)
      require(bool(active[i] & Activity)==(s.actSeq[i]!=0) && bool(active[i] & Recheck)==(s.recheckSeq[i]!=0),
              "missing decision event");
    }
  }

void validateActors(const State& s, const World& w, bool decisions) {
  require(validTime(s.now) && s.now>=w.startTime(),"invalid world time");
  require(std::any_of(s.rng.s.begin(),s.rng.s.end(),[](uint64_t value) { return value!=0; }),"invalid RNG state");
  const auto kindCount = w.config.objectKinds.size();
  std::vector<uint32_t> objects(s.objectUse.size()), points(s.pointUse.size());
  for(uint32_t i=0; i<w.npcs.size(); ++i) {
    require(s.place[i]<w.points.size() && !(s.flags[i] & ~(Moving | AtStart)),"invalid actor placement or flags");
    if(s.flags[i] & Moving) {
      require(s.dest[i]<w.points.size() && s.bodySeq[i]!=0 && validTime(s.depart[i]) && validTime(s.arrive[i]) &&
              s.depart[i]<=s.now && s.now<=s.arrive[i],"invalid actor travel");
      }
    else
      require(s.dest[i]==NoPoint && s.depart[i]==0 && s.arrive[i]==0,"invalid standing actor");
    require(s.entry[i]==entryAt(w,i,s.now) && (s.stateEntry[i]==NoEntry || s.stateEntry[i]<w.entryCount(i)) &&
            (s.state[i]==NoState || s.state[i]<w.stateNames.size()) && validTime(s.stateEnd[i]) &&
            validTime(s.stateStart[i]) && s.stateStart[i]<=s.now,"invalid actor routine");
    if(!s.needSince.empty())
      require(validTime(s.needSince[i]) && s.needSince[i]<=s.now && validTime(s.needAt[i]),"invalid need time");
    if(!decisions)
      continue;
    require(s.mode[i]<=uint8_t(Mode::Chosen) && s.want[i]==0 && s.routineWait[i]<=1 &&
            s.actType[i]<=uint8_t(OptionType::Point) && s.actFinish[i]<=uint8_t(FinishReason::AboveLow),
            "invalid or unsettled decision state");
    require((s.actBegin[i]==-1 || (validTime(s.actBegin[i]) && s.actBegin[i]<=s.now)) &&
            (s.actMin[i]==Never || validTime(s.actMin[i])) && (s.actEnd[i]==Never || validTime(s.actEnd[i])) &&
            (s.forecastFor[i]==-1 || validTime(s.forecastFor[i])),"invalid activity time");
    const auto held = s.routineTarget[i];
    if(held!=NoObject) {
      const auto id = targetId(held);
      const bool point = targetIsPoint(held);
      auto& uses = point ? points : objects;
      require(s.mode[i]==uint8_t(Mode::Routine) && id<uses.size() && s.routineKind[i]<kindCount &&
              (point ? w.points.free[id]!=0 : w.objectKind[id]==s.routineKind[i]),"invalid routine reservation");
      ++uses[id];
      }
    else
      require(s.routineKind[i]==NoKind,"invalid routine reservation kind");
    if(s.mode[i]!=uint8_t(Mode::Chosen)) {
      require(s.actKind[i]==NoKind && s.actSeq[i]==0 && s.deferred[i]==0,"invalid inactive activity");
      continue;
      }
    require(decides(w,i) && s.actPoint[i]<points.size() && s.actKind[i]<kindCount,"invalid chosen activity");
    if(s.actType[i]==uint8_t(OptionType::Object)) {
      const auto id = s.actObject[i];
      require(id<objects.size() && w.objectKind[id]==s.actKind[i] && w.objects.waypoint[id]==s.actPoint[i],
              "invalid chosen object");
      ++objects[id];
      }
    else {
      require(s.actType[i]==uint8_t(OptionType::Point) && s.actObject[i]==NoObject,"invalid chosen point");
      ++points[s.actPoint[i]];
      }
    }
  require(std::equal(points.begin(),points.end(),s.pointUse.begin()) &&
          std::equal(objects.begin(),objects.end(),s.objectUse.begin()),"invalid reservation counts");
  for(size_t i=0; i<s.needLevel.size(); ++i)
    require(s.needLevel[i]>=0 && s.needLevel[i]<=1 && std::isfinite(s.needRate[i]) &&
            std::isfinite(s.needDecay[i]) && s.needDecay[i]>=0 && s.needZone[i]<=2,"invalid need state");
  }

}

std::string serialize(const Sim& sim) {
  const auto& w = *sim.world;
  const auto& s = sim.s;
  require(w.exportHash!=0,"a world export is required");
  require(s.batch.size()==0 && s.asked.empty() &&
          std::none_of(s.want.begin(),s.want.end(),[](uint8_t want) { return want!=0; }),"settle decisions before saving");
  require(sim.drained==s.events.size(),"drain the journal before saving");

  Writer writer{std::string(Magic)};
  writer.scalar(Version);
  writer.scalar(w.exportHash);
  writer.scalar(w.config.resolvedHash);
  writer.scalar(uint8_t(sim.config.decisions));
  writer.scalar(sim.seed);
  transfer(writer,s,w,sim.config.decisions);
  writer.scalar(detail::fnv1a64(writer.bytes));
  return std::move(writer.bytes);
  }

void deserialize(Sim& sim, std::string_view bytes) {
  const auto& w = *sim.world;
  require(w.exportHash!=0,"a world export is required");
  require(bytes.starts_with(Magic) && bytes.size()>=Magic.size()+sizeof(uint64_t),"invalid header");
  uint64_t checksum = 0;
  Reader trailer{bytes.substr(bytes.size()-sizeof(checksum))};
  trailer.scalar(checksum);
  bytes.remove_suffix(sizeof(checksum));
  require(detail::fnv1a64(bytes)==checksum,"checksum mismatch");

  Reader reader{bytes.substr(Magic.size())};
  uint32_t version = 0;
  uint64_t world = 0, config = 0, seed = 0;
  uint8_t decisions = 0;
  reader.scalar(version);
  require(version==Version,"unsupported version");
  reader.scalar(world);
  reader.scalar(config);
  reader.scalar(decisions);
  reader.scalar(seed);
  require(world==w.exportHash && config==w.config.resolvedHash && decisions==uint8_t(sim.config.decisions),
          "world, config or decision mode mismatch");

  State state;
  if(sim.config.decisions)
    state.batch.stride = w.config.decisions.menu;
  transfer(reader,state,w,sim.config.decisions);
  require(reader.bytes.empty(),"trailing data");
  validateActors(state,w,sim.config.decisions);
  validateQueue(state,w,sim.config.decisions);
  sim.s = std::move(state);
  sim.seed = seed;
  sim.drained = 0;
  }

}
