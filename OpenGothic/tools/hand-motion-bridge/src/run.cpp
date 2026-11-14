#include "run.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace Bridge {

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  }

void replay(const std::vector<RecordEvent>& events, Bridge& bridge, const ReplayOptions& o) {
  int64_t first = 0, last = 0;
  bool    any = false;
  // a pass lasts from the first arrival to the last one plus the usual step between packets of a hand
  std::vector<int64_t> steps;
  int64_t prevOf[HandMotion::HandCount] = {};
  bool    seen[HandMotion::HandCount]   = {};
  for(auto& e:events) {
    if(e.isDevice)
      continue;
    const auto& p = e.packet;
    if(!any)
      first = p.arrivalUs;
    any  = true;
    last = std::max(last,p.arrivalUs);
    if(seen[p.hand] && p.arrivalUs>prevOf[p.hand])
      steps.push_back(p.arrivalUs - prevOf[p.hand]);
    prevOf[p.hand] = p.arrivalUs;
    seen[p.hand]   = true;
    }
  if(!any)
    return;
  int64_t step = 15'000;
  if(!steps.empty()) {
    std::nth_element(steps.begin(),steps.begin()+steps.size()/2,steps.end());
    step = steps[steps.size()/2];
    }
  const int64_t passLen = last - first + step;

  const auto    start = Clock::now();
  const int64_t base  = nowUs();
  for(int pass=0; o.passes==0 || pass<o.passes; ++pass) {
    for(auto& e:events) {
      if(o.stop!=nullptr && o.stop->load())
        return;
      if(e.isDevice) {
        bridge.device(e.hand,e.device);
        continue;
        }
      Packet p = e.packet;
      const int64_t since = p.arrivalUs - first + int64_t(pass)*passLen;
      p.arrivalUs = base + since;
      if(o.speed>0) {
        // in short naps, so that a stop during a long pause of the record ends the replay at once
        const auto due = start + std::chrono::microseconds(int64_t(double(since)/o.speed));
        for(auto now=Clock::now(); now<due; now=Clock::now()) {
          if(o.stop!=nullptr && o.stop->load())
            return;
          std::this_thread::sleep_for(std::min<Clock::duration>(due - now,std::chrono::milliseconds(20)));
          }
        if(o.stop!=nullptr && o.stop->load())
          return;
        }
      bridge.feed(p);
      if(o.tick)
        o.tick();
      }
    }
  }

StatusLine::StatusLine(Bridge& b)
  :bridge(b), mark(Clock::now()) {
  }

void StatusLine::tick() {
  const auto now = Clock::now();
  if(now - mark < std::chrono::seconds(1))
    return;
  const double sec = std::chrono::duration<double>(now - mark).count();
  mark = now;
  for(auto h:{HandMotion::Left, HandMotion::Right}) {
    const char*   side = h==HandMotion::Left ? "left " : "right";
    const auto    n    = bridge.takeCount(h);
    const auto&   name = bridge.deviceName(h);
    if(name.empty() && n==0) {
      std::printf("%s  no device\n",side);
      continue;
      }
    const auto& p = bridge.processor(h);
    char ori[96] = "orientation -";
    if(p.quatValid()) {
      double roll = 0, pitch = 0, yaw = 0;
      toEuler(p.orientation(),roll,pitch,yaw);
      std::snprintf(ori,sizeof(ori),"roll %.1f pitch %.1f yaw %.1f",roll,pitch,yaw);
      }
    std::printf("%s  %s  %.0f/s  %s, %s  %s\n", side, name.empty() ? "?" : name.c_str(), double(n)/sec,
                p.biasKnown() ? "bias set" : "no bias yet", p.atRest() ? "at rest" : "moving", ori);
    }
  std::fflush(stdout);
  }

}
