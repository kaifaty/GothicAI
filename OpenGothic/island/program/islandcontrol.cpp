#include "islandcontrol.h"

using namespace Tempest;

namespace island {

namespace {
// degrees per pixel of the mouse
constexpr float MouseTurn = 0.15f;
}

void Keys::key(KeyEvent::KeyType key, bool down) {
  switch(key) {
    case KeyEvent::K_W:      ahead = down; break;
    case KeyEvent::K_S:      back  = down; break;
    case KeyEvent::K_A:      left  = down; break;
    case KeyEvent::K_D:      right = down; break;
    case KeyEvent::K_LShift:
    case KeyEvent::K_RShift: shift = down; break;
    case KeyEvent::K_Space:  if(down) jumpPressed = true; break;
    default: break;
    }
  }

void Keys::mouse(int dx, int dy) {
  turn  += float(dx)*MouseTurn;
  pitch += float(dy)*MouseTurn;
  }

void Keys::clear() {
  *this = Keys();
  }

Control Keys::next(uint64_t, uint64_t) {
  Control c;
  c.forward = float(int(ahead)-int(back));
  c.strafe  = float(int(right)-int(left));
  c.run     = shift;
  c.jump    = jumpPressed;
  c.turn    = turn;
  c.pitch   = pitch;
  jumpPressed = false;
  turn  = 0;
  pitch = 0;
  return c;
  }

Control DemoTrack::next(uint64_t time, uint64_t dt) {
  struct Part {
    uint64_t end;
    float    forward;
    bool     run;
    float    turn; // degrees per second, positive to the right
    };
  static constexpr Part parts[] = {
    { 1000, 0, false,  0},  // a breath: the camera and the exposure settle
    { 6500, 1, true,   0},  // up towards the summit, across the chunk border ahead
    { 8000, 1, true,  60},  // a quarter turn to the right on the run
    {11500, 1, true,   0},  // two jumps on the run
    {12500, 1, false,  0},  // a walk
    {Length, 0, false, 0},  // a stop
    };
  Control c;
  for(const auto& part:parts)
    if(time<part.end) {
      c.forward = part.forward;
      c.run     = part.run;
      c.turn    = part.turn*float(dt)*0.001f;
      break;
      }
  for(const auto jump:Jumps)
    c.jump = c.jump || (time<=jump && jump<time+dt);
  return c;
  }

}
