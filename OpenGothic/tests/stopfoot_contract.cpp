#include "graphics/mesh/stopfoot.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }
  }

int main() {
  // the right foot's frame is the nearer one in the middle half of the loop
  require(!StopFoot::isRight(0.f),    "the loop's start is the left foot's");
  require(!StopFoot::isRight(0.24f),  "a quarter in is still the left foot's");
  require( StopFoot::isRight(0.25f),  "from a quarter on the right foot is nearer");
  require( StopFoot::isRight(0.5f),   "the half is the right foot's");
  require( StopFoot::isRight(0.74f),  "up to three quarters the right foot is nearer");
  require(!StopFoot::isRight(0.75f),  "the last quarter leads back to the left foot");

  // our run loop: 21 frames a cycle at 29.56 frames a second, 710 ms
  const float fps = 29.56f;
  require(std::abs(StopFoot::phase(0,fps,21))<1e-4f,           "a loop starts at phase 0");
  require(std::abs(StopFoot::phase(355,fps,21)-0.5f)<0.01f,    "half a cycle is phase 0.5");
  require(std::abs(StopFoot::phase(710+355,fps,21)-0.5f)<0.01f,"the phase wraps over the cycles");
  require(StopFoot::phase(100,0.f,21)==0.f,                    "no rate: phase 0");
  require(StopFoot::phase(100,fps,0)==0.f,                     "no frames: phase 0");

  require(StopFoot::rightName("RUNL")=="RUNR",       "RUNL -> RUNR");
  require(StopFoot::rightName("1HWALKL")=="1HWALKR", "1HWALKL -> 1HWALKR");
  require(StopFoot::rightName("RUN").empty(),        "a stance has no right variant");
  require(StopFoot::rightName("L").empty(),          "a bare L is not a loop");
  require(StopFoot::rightName("").empty(),           "an empty name has no variant");

  // a stop at 30 frames a second: 11, 8, 6, 3, 2 cm a frame, then it creeps back by 0.5 cm a frame (15 cm/s);
  // from frame 5 on the root is slower than 35 cm/s
  std::vector<std::array<float,2>> way = {{0,0},{0,11},{0,19},{0,25},{0,28},{0,30},{0,29.5f},{0,29}};
  require(StopFoot::restFrame(way,0,28.5f,30.f)==5, "the root rests from the first frame it stays slow from");
  require(StopFoot::restFrame(way,0,28.5f,100.f)==9,"the same steps at 100 frames a second are 50 cm/s: no rest");
  std::vector<std::array<float,2>> run = {{0,0},{0,10},{0,20}};
  require(StopFoot::restFrame(run,0,30,30.f)==4,    "a clip that never rests: the number of its frames");
  std::vector<std::array<float,2>> stand = {{0,0},{0,0.2f}};
  require(StopFoot::restFrame(stand,0,0,25.f)==0,   "a clip in place rests from the start");

  std::puts("stopfoot contract ok");
  return 0;
  }
