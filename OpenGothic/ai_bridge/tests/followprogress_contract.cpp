#include "followprogress.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }
}

int main() {
  using P = FollowProgress;
  const P::Point leader{20730,2000,-20850};
  const std::array<P::Point,16> oscillation{{
    {20746,2902,-22096}, {20715,2902,-22395},
    {20741,2904,-22134}, {20715,2902,-22395},
    {20746,2902,-22096}, {20715,2902,-22395},
    {20741,2904,-22134}, {20715,2902,-22395},
    {20746,2902,-22096}, {20715,2902,-22395},
    {20741,2904,-22134}, {20715,2902,-22395},
    {20746,2902,-22096}, {20715,2902,-22395},
    {20741,2904,-22134}, {20715,2902,-22395},
    }};
  P rocking;
  auto firstStuck = std::numeric_limits<uint64_t>::max();
  for(size_t i=0; i<oscillation.size(); ++i) {
    const uint64_t tick = static_cast<uint64_t>(i)*1000;
    if(rocking.update(tick,oscillation[i],leader) && firstStuck==std::numeric_limits<uint64_t>::max())
      firstStuck = tick;
    }
  require(firstStuck<=3000,"three-metre oscillation near a stationary leader detects within three seconds");
  require(firstStuck>=P::StuckMs,"oscillation retains the initial no-progress grace period");

  P approaching;
  for(uint64_t tick=0; tick<=12000; tick+=1000) {
    const float distance = static_cast<float>(tick)*0.33f;
    require(!approaching.update(tick,{distance,0,0},{10000,0,0}),
            "running towards a stationary leader at 3.3 metres per second never detects");
    }

  P following;
  for(uint64_t tick=0; tick<=12000; tick+=1000) {
    const float distance = static_cast<float>(tick)*0.33f;
    require(!following.update(tick,{distance,0,0},{10000+distance,0,0}),
            "running behind a moving leader at a constant distance never detects");
    }

  P sideways;
  for(uint64_t tick=0; tick<=12000; tick+=1000) {
    const float distance = static_cast<float>(tick)*0.33f;
    require(!sideways.update(tick,{0,0,distance},{10000,0,0},true),
            "a waynet detour sideways from a stationary leader never detects");
    }
  require(!sideways.update(13000,{0,0,4290},{10000,0,0}),
          "returning to direct follow retains the last progress grace period");
  require(sideways.update(14000,{0,0,4620},{10000,0,0}),
          "returning to direct follow requires distance progress again");

  P away;
  for(uint64_t tick=0; tick<=12000; tick+=1000) {
    const float distance = static_cast<float>(tick)*0.33f;
    require(!away.update(tick,{-distance,0,0},{10000,0,0},true),
            "a waynet detour away from a stationary leader never detects");
    }

  P waynetStill;
  require(!waynetStill.update(0,{10,20,30},{1000,20,30},true),
          "a waynet follower starts with a fresh timeout");
  require(!waynetStill.update(P::StuckMs-1,{10,20,30},{1000,20,30},true),
          "a stationary waynet follower waits for the full timeout");
  require(waynetStill.update(P::StuckMs,{10,20,30},{1000,20,30},true),
          "a stationary waynet follower still detects at the timeout");

  P still;
  require(!still.update(0,{10,20,30},{1000,20,30}),"the initial position at time zero is not stuck");
  require(!still.update(P::StuckMs-1,{10,20,30},{1000,20,30}),"a stationary follower waits for the full timeout");
  require(still.update(P::StuckMs,{10,20,30},{1000,20,30}),"a stationary follower detects at the timeout");
  require(still.idleMs(P::StuckMs)==P::StuckMs,"idle time includes the initial zero-time sample");

  P paused;
  for(uint64_t tick=0; tick<=10000; tick+=1000) {
    paused.reset(tick,{0,0,0},{300,0,0});
    require(paused.idleMs(tick)==0 && !paused.update(tick,{0,0,0},{300,0,0}),
            "waiting near the leader keeps the no-progress clock reset");
    }
  paused.reset(11000,{0,0,0},{1500,0,0});
  require(paused.idleMs(11000)==0,"the idle tick after the leader moves grants a fresh pursuit timeout");
  require(!paused.update(12000,{0,0,0},{1500,0,0}),
          "one second of turning after a pause is not stuck");
  require(!paused.update(11000+P::StuckMs-1,{0,0,0},{1500,0,0}),
          "pursuit after a pause waits for the full no-progress timeout");
  require(paused.update(11000+P::StuckMs,{0,0,0},{1500,0,0}),
          "two stationary walking seconds after a pause still detect a blockage");

  P threshold;
  require(!threshold.update(0,{0,0,0},{1000,0,0}),"the distance milestone starts with the first sample");
  require(!threshold.update(P::StuckMs,{P::MovedCm,0,0},{1000,0,0}),
          "exactly seventy-five centimetres closer counts as progress at the timeout");
  require(threshold.idleMs(P::StuckMs)==0,"distance progress resets idle time");
  require(!threshold.update(2*P::StuckMs-1,{2*P::MovedCm-1,0,0},{1000,0,0}),
          "less than seventy-five centimetres closer retains the grace period");
  require(threshold.update(2*P::StuckMs,{2*P::MovedCm-1,0,0},{1000,0,0}),
          "less than seventy-five centimetres closer does not reset progress");

  P leaderThreshold;
  require(!leaderThreshold.update(0,{0,0,0},{1000,0,0}),"a new leader-motion window is not stuck");
  require(leaderThreshold.update(P::StuckMs,{-P::MovedCm,0,0},{1000+P::MovedCm,0,0}),
          "a leader moving exactly seventy-five centimetres does not enable displacement progress");

  still.reset(5000,{250,20,30},{1000,20,30});
  require(still.idleMs(5000)==0,"an explicit reset clears an earlier stationary timeout");
  require(!still.update(5000+P::StuckMs-1,{250,20,30},{1000,20,30}),"an explicit reset grants a new timeout");
  require(still.update(5000+P::StuckMs,{250,20,30},{1000,20,30}),"the reset window can detect another stop");
  std::printf("Follow progress contract: %d checks passed\n",checks);
  }
