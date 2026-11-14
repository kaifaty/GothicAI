#pragma once

// The processing of the hand-motion bridge (README.md): a packet of raw sensor samples of one hand in, one
// frame per sample out. Time, gyro bias and orientation live here; nothing here knows SDL or sockets.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "input/handmotionframe.h"

namespace Bridge {

inline constexpr double Gravity = 9.80665; // m/s², SDL_STANDARD_GRAVITY
inline constexpr double Deg     = 3.14159265358979323846/180.0;

// rest: over RestWindow every gyro axis spreads (standard deviation) less than RestGyroSd and every |accel| is
// within RestAccel of Gravity
inline constexpr int64_t RestWindowUs = 1'000'000;
inline constexpr int64_t RestMinSpanUs = 950'000;  // the window has to cover this much to count
inline constexpr double  RestGyroSd   = 0.5*Deg;   // rad/s
inline constexpr double  RestAccel    = 0.3;       // m/s²
// after the first estimate the bias follows the window's mean with this time constant, only at rest
inline constexpr double  BiasTauS     = 10.0;
// Mahony: the gravity correction works only while |accel| is within GravityGate of 1 g
inline constexpr double  GravityGate  = 0.1*Gravity;
inline constexpr double  MahonyKp     = 1.0;       // 1/s
// a hand silent this long has lost its stream: the orientation starts again from gravity
inline constexpr int64_t StreamGapUs  = 500'000;
// the filter integrates over the sensor's step; a step outside (0, this] (a new device, a reset sensor clock) falls
// back to the step of the frames' times
inline constexpr int64_t MaxSensorStepUs = 100'000;

struct RawSample {
  int64_t  sensorUs = 0;  // the sensor's time from SDL, µs: only its steps are used, never the value itself
  float    gyro[3]  = {}; // rad/s, SDL gamepad axes
  float    accel[3] = {}; // m/s²
  uint32_t buttons  = 0;  // bit N: SDL_GamepadButton N
  };

// samples that came in one report: SDL gave them one event time (within 1 ms)
struct Packet {
  HandMotion::Hand       hand      = HandMotion::Left;
  int64_t                arrivalUs = 0; // the newest sample's arrival, steady_clock µs
  std::vector<RawSample> samples;       // oldest first
  };

struct Quat {
  double w = 1, x = 0, y = 0, z = 0;
  };

// roll about X, pitch about Y, yaw about Z (Z up), degrees: the receiver's log uses the same
void toEuler(const Quat& q, double& roll, double& pitch, double& yaw);
// the orientation of a sensor at rest that measures `accel`: roll and pitch from gravity, yaw 0
Quat fromGravity(const double accel[3]);

// one sample, ready to be a frame
struct Processed {
  uint64_t tUs      = 0;
  uint8_t  flags    = 0;
  uint32_t buttons  = 0;
  float    gyro[3]  = {}; // bias removed when flags has GyroBiasRemoved
  float    accel[3] = {};
  float    quat[4]  = {1,0,0,0};
  };

class HandProcessor final {
  public:
    // appends one Processed per sample of the packet
    void process(const Packet& p, std::vector<Processed>& out);
    // another device took the hand: everything but the clock starts again
    void reset();

    bool     biasKnown() const { return hasBias; }
    bool     atRest() const { return rest; }
    bool     quatValid() const { return hasBias && hasQuat; }
    Quat     orientation() const { return q; }
    const double* bias() const { return gyroBias; }
    // packets whose sensor steps did not fit between the previous sample and the arrival: squeezed evenly
    uint64_t squeezed() const { return squeezedPackets; }

  private:
    struct WindowSample {
      int64_t tUs;
      double  gyro[3];
      double  accel[3];
      bool    accelOk;
      };

    void stamp(const Packet& p, std::vector<int64_t>& t);
    void updateRest(const WindowSample& s);
    void integrate(const double gyro[3], const double accel[3], double dt);

    bool     anyTime = false;
    int64_t  lastUs  = 0;
    int64_t  lastSensorUs = 0;
    uint64_t squeezedPackets = 0;

    std::deque<WindowSample> window;
    bool     rest = false;
    double   windowMeanGyro[3]  = {};
    double   windowMeanAccel[3] = {};

    bool     hasBias = false;
    double   gyroBias[3] = {};

    bool     hasQuat = false;
    Quat     q;
  };

// the session, the hands' seq and processors: packets in, encoded frames out to a sink
class FrameSink {
  public:
    virtual ~FrameSink() = default;
    virtual void send(const HandMotion::Frame& f) = 0;
  };

HandMotion::Frame makeFrame(HandMotion::Hand hand, uint16_t session, uint32_t seq, const Processed& s);

class Bridge final {
  public:
    Bridge(uint16_t session, FrameSink& sink);

    // a device took the hand; a device with another name than the last one starts the hand's processing again
    void device(HandMotion::Hand hand, const std::string& name);
    void feed(const Packet& p);

    uint16_t                session() const { return sess; }
    const HandProcessor&    processor(HandMotion::Hand h) const { return hands[h].proc; }
    const std::string&      deviceName(HandMotion::Hand h) const { return hands[h].name; }
    uint32_t                nextSeq(HandMotion::Hand h) const { return hands[h].seq; }
    // frames sent since the last call
    uint64_t                takeCount(HandMotion::Hand h);

  private:
    struct Hand {
      HandProcessor proc;
      std::string   name;
      uint32_t      seq   = 0;
      uint64_t      count = 0;
      };

    uint16_t               sess;
    FrameSink&             sink;
    Hand                   hands[HandMotion::HandCount];
    std::vector<Processed> scratch;
  };

}
