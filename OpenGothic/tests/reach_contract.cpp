#include "game/reachsignal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
  int failures = 0;

  void check(bool condition, const char* name) {
    if(!condition) {
      std::fprintf(stderr,"FAIL: %s\n",name);
      ++failures;
      }
    }

  bool near(float a, float b, float tolerance = 0.001f) {
    return std::isfinite(a) && std::abs(a-b)<=tolerance;
    }

  // a body running at 332 cm/s, 35 cm of capsule, 10 ms a move
  ReachSignal::Input running() {
    ReachSignal::Input input;
    input.dtMs     = 10;
    input.eligible = true;
    input.moving   = true;
    input.avgSpeed = 332;
    input.radius   = 35;
    return input;
    }

  ReachSignal::Input wallAt(float distance, float angle = 0.f) {
    auto input = running();
    input.ahead    = true;
    input.distance = distance;
    input.cosine   = std::cos(angle*(3.14159265358979323846f/180.f));
    return input;
    }

  // held by an obstacle: `still` of the asked move is not gone, forward or sideways
  ReachSignal::Input held(float still) {
    auto input = running();
    input.obstacle = true;
    input.contact  = true;
    input.moved    = 1.f-still;
    return input;
    }

  float pressAfter(const ReachSignal::Input& input, int moves, ReachSignal::State state = {}) {
    float press = 0;
    for(int i=0; i<moves; ++i)
      press = ReachSignal::update(state,input).press;
    return press;
    }
  }

int main() {
  // the warning: a quarter of a second ahead of the capsule's contact, not before
  {
    ReachSignal::State state;
    // 35 cm of capsule + 332*0.25 = 83 cm of way: the wall's ray is 118 cm long at the threshold
    const auto far  = ReachSignal::update(state,wallAt(119.f));
    check(!far.warn && near(far.timeToHit,84.f/332.f),"warn_far_off");
    const auto near_ = ReachSignal::update(state,wallAt(117.f));
    check(near_.warn && near(near_.timeToHit,82.f/332.f) && near(near_.closing,332.f),"warn_quarter_second");
    std::printf("warn: 119 cm -> %d (%.3f s), 117 cm -> %d (%.3f s)\n",far.warn,far.timeToHit,near_.warn,near_.timeToHit);
  }
  // given, it holds on a little further and slower than it starts
  {
    ReachSignal::State state;
    ReachSignal::update(state,wallAt(100.f));
    check(ReachSignal::update(state,wallAt(150.f)).warn,"warn_holds_to_keep_time");
    check(!ReachSignal::update(state,wallAt(152.f)).warn,"warn_lets_go_past_keep_time");
    ReachSignal::State fresh;
    check(!ReachSignal::update(fresh,wallAt(150.f)).warn,"warn_does_not_start_at_keep_time");
  }
  // the closing speed is what counts: 45 degrees closes at 235 cm/s, 60 at 166 - below the answer's threshold
  {
    ReachSignal::State a, b;
    const auto at45 = ReachSignal::update(a,wallAt(60.f,45.f));
    const auto at60 = ReachSignal::update(b,wallAt(60.f,60.f));
    check(at45.warn && near(at45.closing,234.76f,0.05f),"warn_45_degrees");
    check(!at60.warn && near(at60.closing,166.f,0.05f),"no_warn_60_degrees");
    std::printf("angle: 45 -> %d (%.1f cm/s), 60 -> %d (%.1f cm/s)\n",at45.warn,at45.closing,at60.warn,at60.closing);
  }
  // a walk does not look ahead; a body that slides on after the contact does not either
  {
    ReachSignal::State state;
    auto walk = wallAt(50.f);
    walk.avgSpeed = 196;
    check(!ReachSignal::wantsProbe(walk) && !ReachSignal::update(state,walk).warn,"no_warn_walking");
    auto slide = wallAt(40.f,45.f);
    slide.contact = true;
    check(!ReachSignal::wantsProbe(slide) && !ReachSignal::update(state,slide).warn,"no_warn_in_contact");
    auto still = wallAt(50.f);
    still.moving = false;
    check(!ReachSignal::wantsProbe(still),"no_probe_standing");
    check(near(ReachSignal::probeLength(332.f,35.f),35.f+332.f*0.35f),"probe_length");
  }
  // pressed: the stand comes in 150 ms, by how much of the move is lost
  {
    check(near(pressAfter(held(1.f),15),1.f),"press_full_150_ms");
    check(near(pressAfter(held(1.f),7),7.f/15.f,0.002f),"press_linear_rise");
    // a wall met at 45 degrees leaves cos 45 of the move as a slide, one met at 10 degrees sin 10
    check(near(pressAfter(held(1.f-0.707f),40),0.f),"no_press_sliding_at_45");
    check(pressAfter(held(1.f-0.174f),40)>0.95f,"press_at_10_degrees");
    check(near(pressAfter(held(0.55f),40),0.f),"press_threshold_low");
    check(near(pressAfter(held(0.85f),40),1.f),"press_threshold_full");
    check(near(pressAfter(held(0.70f),40),0.5f),"press_half_between");
    // let go: back in 150 ms
    ReachSignal::State state;
    pressAfter(held(1.f),15,state);
    for(int i=0; i<15; ++i)
      ReachSignal::update(state,held(1.f));
    auto free = running();
    float press = 1;
    for(int i=0; i<8; ++i)
      press = ReachSignal::update(state,free).press;
    check(near(press,7.f/15.f,0.002f),"press_falls_linear");
    // a loss with nothing against the move (a slope, a step) is not a press
    auto slope = running();
    slope.moved = 0.f;
    check(near(pressAfter(slope,40),0.f),"no_press_without_obstacle");
    std::printf("press: full %.2f, half-way %.2f, at 45 %.2f\n",pressAfter(held(1.f),15),pressAfter(held(0.70f),40),
                pressAfter(held(1.f-0.707f),40));
  }
  // the hands: out with the warning, through the contact's first moves (the press is not asked yet), on while held
  {
    ReachSignal::State state;
    check(ReachSignal::update(state,wallAt(100.f)).hands,"hands_with_warning");
    auto partly = held(0.4f); // the contact's own move: a part of it is still gone
    bool through = true;
    for(int i=0; i<14; ++i)
      through = through && ReachSignal::update(state,partly).hands;
    check(through,"hands_hold_through_contact");
    for(int i=0; i<3; ++i)
      ReachSignal::update(state,partly);
    check(!ReachSignal::update(state,partly).hands,"hands_let_go_150_ms_after_warning");
    ReachSignal::State walk;
    check(ReachSignal::update(walk,held(1.f)).hands && ReachSignal::update(walk,held(1.f)).plant,"hands_when_held_without_warning");
    check(!ReachSignal::update(walk,held(1.f-0.707f)).hands,"no_hands_sliding_at_45");
  }
  // the gates
  {
    auto standing = held(1.f);
    standing.moving = false;
    check(near(pressAfter(standing,40),0.f),"no_press_standing");
    auto fighting = held(1.f);
    fighting.eligible = false;
    check(near(pressAfter(fighting,40),0.f),"no_press_ineligible");
    auto ineligibleWarn = wallAt(60.f);
    ineligibleWarn.eligible = false;
    ReachSignal::State state;
    check(!ReachSignal::update(state,ineligibleWarn).warn,"no_warn_ineligible");
    // the flag off: nothing, and the state is dropped
    ReachSignal::State on;
    pressAfter(held(1.f),15,on);
    auto off = held(1.f);
    off.enabled = false;
    const auto result = ReachSignal::update(on,off);
    check(near(result.press,0.f) && !result.warn && near(on.press,0.f),"flag_disabled");
  }
  {
    // the wall stop: a wall that stands flat at every look to both sides of the body, 8 cm of capsule
    using Look = ReachSignal::WallLook;
    const float radius = 8.f;
    const Look planks[4] = {{true,1.f,24.f},{true,0.97f,23.f},{true,1.f,23.5f},{true,0.95f,26.f}};
    check(ReachSignal::flatWall(planks,4,radius),"flat_planks");
    // a door or the house's corner within the reach to a side: nothing there, or a thing that is not a wall
    Look door[4] = {planks[0],planks[1],planks[2],{false,0.f,48.f}};
    check(!ReachSignal::flatWall(door,4,radius),"not_flat_opening");
    // a niche or a post: the wall steps further than planks do
    Look niche[4] = {planks[0],planks[1],planks[2],{true,1.f,38.f}};
    check(!ReachSignal::flatWall(niche,4,radius),"not_flat_step");
    // an inner corner: the wall to a side faces another way
    Look corner[4] = {{true,0.2f,20.f},planks[1],planks[2],planks[3]};
    check(!ReachSignal::flatWall(corner,4,radius),"not_flat_turned");
    // the body is held by something that stands in front of the wall
    const Look far[4] = {{true,1.f,36.f},{true,1.f,36.f},{true,1.f,36.f},{true,1.f,36.f}};
    check(!ReachSignal::flatWall(far,4,radius),"not_flat_far");
    check(!ReachSignal::flatWall(planks,0,radius),"not_flat_unseen");

    const auto into = [](float degrees) { return std::cos(degrees*(3.14159265358979323846f/180.f)); };
    check(ReachSignal::stops(false,true,into(0.f)) && ReachSignal::stops(false,true,into(13.f)) &&
          ReachSignal::stops(false,true,into(24.f)),"stop_head_on");
    check(!ReachSignal::stops(false,true,into(27.f)) && !ReachSignal::stops(false,true,into(45.f)),"slide_past_25");
    // a stop begun holds on to 30 degrees: a hero who turns away is let go once
    check(ReachSignal::stops(true,true,into(27.f)) && !ReachSignal::stops(true,true,into(32.f)),"stop_keeps_to_30");
    check(!ReachSignal::stops(false,false,into(0.f)) && !ReachSignal::stops(true,false,into(0.f)),"no_stop_not_flat");
    // the move asks away from the wall
    check(!ReachSignal::stops(true,true,-1.f),"no_stop_away");
  }
  if(failures!=0)
    return EXIT_FAILURE;
  std::puts("reach contract passed");
  }
