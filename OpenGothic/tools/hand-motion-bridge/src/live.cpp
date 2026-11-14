#include "run.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace Bridge {

namespace {

// One opened gamepad. SDL gives the gyro and the accelerometer of a sample as two events with the same sensor
// time; samples whose SDL event times lie within 1 ms came in one report and make one packet.
struct Pad {
  SDL_JoystickID   id   = 0;
  SDL_Gamepad*     pad  = nullptr;
  HandMotion::Hand hand = HandMotion::Left;
  std::string      name;
  uint32_t         buttons = 0;

  bool     pending   = false;
  uint64_t pendSensorNs = 0;
  uint64_t pendEventNs  = 0;
  bool     hasGyro = false, hasAccel = false;
  float    gyro[3] = {}, accel[3] = {};

  struct Timed {
    uint64_t  sensorNs;
    uint64_t  eventNs;
    RawSample s;
    };
  std::vector<Timed> packet;
  };

class Live final {
  public:
    Live(Bridge& b, RecordWriter* r):bridge(b), rec(r) {}
    ~Live() {
      for(auto& p:pads)
        SDL_CloseGamepad(p->pad);
      }

    // SDL's event time onto steady_clock: "now by SDL - now by steady_clock", before every drain
    void sync();
    void event(const SDL_Event& e);
    // everything SDL gave in this drain is whole: complete the samples and send the packets
    void flush();

  private:
    Pad* find(SDL_JoystickID id) {
      for(auto& p:pads)
        if(p->id==id)
          return p.get();
      return nullptr;
      }
    bool taken(HandMotion::Hand h) const {
      for(auto& p:pads)
        if(p->hand==h)
          return true;
      return false;
      }
    void added(SDL_JoystickID id);
    void removed(SDL_JoystickID id);
    void sensor(Pad& p, const SDL_GamepadSensorEvent& e);
    void complete(Pad& p);
    void send(Pad& p);

    Bridge&                           bridge;
    RecordWriter*                     rec;
    std::vector<std::unique_ptr<Pad>> pads;
    int64_t                           sdlToSteadyNs = 0;
  };

const char* side(HandMotion::Hand h) {
  return h==HandMotion::Left ? "left" : "right";
  }

void Live::event(const SDL_Event& e) {
  switch(e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      added(e.gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      removed(e.gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
      if(Pad* p = find(e.gsensor.which))
        sensor(*p,e.gsensor);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      if(Pad* p = find(e.gbutton.which); p!=nullptr && e.gbutton.button<32) {
        const uint32_t bit = 1u<<e.gbutton.button;
        p->buttons = e.gbutton.down ? (p->buttons | bit) : (p->buttons & ~bit);
        }
      break;
    default:
      break;
    }
  }

// Joy-Con (L) is the left hand, (R) the right one; another gamepad with both sensors takes the first free hand,
// the right one first
void Live::added(SDL_JoystickID id) {
  if(find(id)!=nullptr)
    return;
  SDL_Gamepad* gp = SDL_OpenGamepad(id);
  if(gp==nullptr) {
    std::printf("cannot open gamepad %u: %s\n",unsigned(id),SDL_GetError());
    return;
    }
  const char* nm = SDL_GetGamepadName(gp);
  std::string name = nm!=nullptr ? nm : "gamepad";
  if(!SDL_GamepadHasSensor(gp,SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(gp,SDL_SENSOR_ACCEL)) {
    std::printf("%s: no gyroscope or accelerometer, skipped\n",name.c_str());
    SDL_CloseGamepad(gp);
    return;
    }
  HandMotion::Hand hand = HandMotion::Right;
  switch(SDL_GetGamepadType(gp)) {
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
      hand = HandMotion::Left;
      break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
      hand = HandMotion::Right;
      break;
    default:
      hand = taken(HandMotion::Right) ? HandMotion::Left : HandMotion::Right;
      break;
    }
  if(taken(hand)) {
    std::printf("%s: the %s hand is taken, skipped\n",name.c_str(),side(hand));
    SDL_CloseGamepad(gp);
    return;
    }
  if(!SDL_SetGamepadSensorEnabled(gp,SDL_SENSOR_GYRO,true) || !SDL_SetGamepadSensorEnabled(gp,SDL_SENSOR_ACCEL,true)) {
    std::printf("%s: cannot turn the sensors on: %s\n",name.c_str(),SDL_GetError());
    SDL_CloseGamepad(gp);
    return;
    }
  auto p = std::make_unique<Pad>();
  p->id   = id;
  p->pad  = gp;
  p->hand = hand;
  p->name = name;
  std::printf("%s hand: %s, sensors at %.0f Hz\n",side(hand),name.c_str(),
              double(SDL_GetGamepadSensorDataRate(gp,SDL_SENSOR_GYRO)));
  bridge.device(hand,name);
  if(rec!=nullptr)
    rec->device(hand,name);
  pads.push_back(std::move(p));
  }

void Live::removed(SDL_JoystickID id) {
  for(size_t i=0; i<pads.size(); ++i) {
    Pad& p = *pads[i];
    if(p.id!=id)
      continue;
    complete(p);
    send(p);
    std::printf("%s hand: %s disconnected\n",side(p.hand),p.name.c_str());
    SDL_CloseGamepad(p.pad);
    pads.erase(pads.begin()+ptrdiff_t(i));
    return;
    }
  }

void Live::sensor(Pad& p, const SDL_GamepadSensorEvent& e) {
  const bool gyro = e.sensor==SDL_SENSOR_GYRO;
  if(!gyro && e.sensor!=SDL_SENSOR_ACCEL)
    return;
  if(p.pending && p.pendSensorNs!=e.sensor_timestamp)
    complete(p);
  if(!p.pending) {
    p.pending      = true;
    p.pendSensorNs = e.sensor_timestamp;
    p.pendEventNs  = e.timestamp;
    p.hasGyro      = false;
    p.hasAccel     = false;
    }
  float* dst = gyro ? p.gyro : p.accel;
  for(int i=0; i<3; ++i)
    dst[i] = e.data[i];
  (gyro ? p.hasGyro : p.hasAccel) = true;
  if(p.hasGyro && p.hasAccel)
    complete(p);
  }

// a sample without one of its halves keeps that sensor's last values
void Live::complete(Pad& p) {
  if(!p.pending)
    return;
  p.pending = false;
  if(!p.packet.empty() && p.pendEventNs - p.packet.back().eventNs>=1'000'000)
    send(p);
  Pad::Timed t;
  t.sensorNs = p.pendSensorNs;
  t.eventNs  = p.pendEventNs;
  for(int i=0; i<3; ++i) {
    t.s.gyro[i]  = p.gyro[i];
    t.s.accel[i] = p.accel[i];
    }
  t.s.buttons = p.buttons;
  p.packet.push_back(t);
  }

void Live::send(Pad& p) {
  if(p.packet.empty())
    return;
  const auto& newest = p.packet.back();
  Packet pk;
  pk.hand      = p.hand;
  pk.arrivalUs = (int64_t(newest.eventNs) - sdlToSteadyNs)/1000;
  pk.samples.reserve(p.packet.size());
  for(auto& t:p.packet) {
    RawSample s = t.s;
    s.sensorUs = int64_t(t.sensorNs/1000);
    pk.samples.push_back(s);
    }
  p.packet.clear();
  if(rec!=nullptr)
    rec->packet(pk);
  bridge.feed(pk);
  }

void Live::sync() {
  const int64_t sdlNow    = int64_t(SDL_GetTicksNS());
  const int64_t steadyNow = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now().time_since_epoch()).count();
  sdlToSteadyNs = sdlNow - steadyNow;
  }

void Live::flush() {
  for(auto& p:pads) {
    complete(*p);
    send(*p);
    }
  }

}

int runLive(Bridge& bridge, RecordWriter* rec, const std::atomic<bool>& stop, bool status) {
  // every Joy-Con is a gamepad of its own
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS,"0");
  if(!SDL_Init(SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
    return 1;
    }
  const int v = SDL_GetVersion();
  std::printf("SDL %d.%d.%d, session %u; waiting for gamepads\n",SDL_VERSIONNUM_MAJOR(v),SDL_VERSIONNUM_MINOR(v),
              SDL_VERSIONNUM_MICRO(v),unsigned(bridge.session()));
  std::fflush(stdout);
  {
    Live       live(bridge,rec);
    StatusLine line(bridge);
    auto       flushMark = Clock::now();
    bool       quit = false;
    while(!quit && !stop.load()) {
      live.sync();
      SDL_Event e;
      while(SDL_PollEvent(&e)) {
        if(e.type==SDL_EVENT_QUIT)
          quit = true;
        live.event(e);
        }
      live.flush();
      if(status)
        line.tick();
      if(rec!=nullptr && Clock::now() - flushMark>=std::chrono::seconds(1)) {
        rec->flush();
        flushMark = Clock::now();
        }
      SDL_Delay(1);
      }
  }
  if(rec!=nullptr)
    rec->flush();
  SDL_Quit();
  return 0;
  }

}
