#include "motion.h"

#include <algorithm>
#include <cmath>

namespace Bridge {

namespace {

Quat mul(const Quat& a, const Quat& b) {
  return {
    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
    };
  }

double norm3(const double v[3]) {
  return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
  }

}

void toEuler(const Quat& q, double& roll, double& pitch, double& yaw) {
  constexpr double deg = 180.0/3.14159265358979323846;
  roll  = std::atan2(2*(q.w*q.x + q.y*q.z), 1 - 2*(q.x*q.x + q.y*q.y))*deg;
  pitch = std::asin(std::clamp(2*(q.w*q.y - q.z*q.x), -1.0, 1.0))*deg;
  yaw   = std::atan2(2*(q.w*q.z + q.x*q.y), 1 - 2*(q.y*q.y + q.z*q.z))*deg;
  }

Quat fromGravity(const double accel[3]) {
  // a sensor at rest measures "up" in its own axes: (-sin p, sin r cos p, cos r cos p) for q = Ry(p)*Rx(r)
  const double roll  = std::atan2(accel[1],accel[2]);
  const double pitch = std::atan2(-accel[0],std::sqrt(accel[1]*accel[1] + accel[2]*accel[2]));
  const double cr = std::cos(roll/2),  sr = std::sin(roll/2);
  const double cp = std::cos(pitch/2), sp = std::sin(pitch/2);
  return {cr*cp, sr*cp, cr*sp, -sr*sp};
  }

void HandProcessor::reset() {
  window.clear();
  rest    = false;
  hasBias = false;
  hasQuat = false;
  for(auto& b:gyroBias)
    b = 0;
  q = Quat();
  }

// The newest sample of a packet gets the arrival time; the earlier ones go back by their sensor steps. When those
// steps do not fit after the hand's previous sample (the previous packet came late), the packet is spread evenly
// between the previous sample and the arrival, so that the hand's time still grows strictly.
void HandProcessor::stamp(const Packet& p, std::vector<int64_t>& t) {
  const size_t  n      = p.samples.size();
  const int64_t newest = p.samples[n-1].sensorUs;
  t.resize(n);
  bool fits = true;
  for(size_t i=0; i<n; ++i) {
    t[i] = p.arrivalUs - (newest - p.samples[i].sensorUs);
    if(i>0 ? t[i]<=t[i-1] : anyTime && t[i]<=lastUs)
      fits = false;
    }
  if(!fits) {
    ++squeezedPackets;
    // the first packet with disordered sensor times: a nominal 5 ms step
    const int64_t lo   = anyTime ? lastUs : p.arrivalUs - int64_t(n)*5000;
    const int64_t span = p.arrivalUs - lo;
    for(size_t i=0; i<n; ++i)
      t[i] = span>=int64_t(n) ? lo + span*int64_t(i+1)/int64_t(n) : lo + int64_t(i) + 1;
    }
  lastUs  = t[n-1];
  anyTime = true;
  }

void HandProcessor::updateRest(const WindowSample& s) {
  window.push_back(s);
  while(window.size()>1 && s.tUs - window.front().tUs > RestWindowUs)
    window.pop_front();
  rest = false;
  if(s.tUs - window.front().tUs < RestMinSpanUs)
    return;
  double sum[3] = {}, acc[3] = {};
  for(auto& w:window) {
    if(!w.accelOk)
      return;
    for(int i=0; i<3; ++i) {
      sum[i] += w.gyro[i];
      acc[i] += w.accel[i];
      }
    }
  const double n = double(window.size());
  double var[3] = {};
  for(auto& w:window)
    for(int i=0; i<3; ++i) {
      const double d = w.gyro[i] - sum[i]/n;
      var[i] += d*d;
      }
  for(int i=0; i<3; ++i)
    if(std::sqrt(var[i]/n)>=RestGyroSd)
      return;
  rest = true;
  for(int i=0; i<3; ++i) {
    windowMeanGyro[i]  = sum[i]/n;
    windowMeanAccel[i] = acc[i]/n;
    }
  }

// Mahony: the gyro turns the orientation, the gravity pulls its "up" towards the measured one
void HandProcessor::integrate(const double gyro[3], const double accel[3], double dt) {
  double g[3] = {gyro[0], gyro[1], gyro[2]};
  const double n = norm3(accel);
  if(n>0 && std::abs(n - Gravity)<=GravityGate) {
    const double a[3] = {accel[0]/n, accel[1]/n, accel[2]/n};
    // world Z in the sensor's axes
    const double v[3] = {
      2*(q.x*q.z - q.w*q.y),
      2*(q.w*q.x + q.y*q.z),
      q.w*q.w - q.x*q.x - q.y*q.y + q.z*q.z,
      };
    g[0] += MahonyKp*(a[1]*v[2] - a[2]*v[1]);
    g[1] += MahonyKp*(a[2]*v[0] - a[0]*v[2]);
    g[2] += MahonyKp*(a[0]*v[1] - a[1]*v[0]);
    }
  const double w = norm3(g);
  if(w<=1e-12)
    return;
  const double half = 0.5*w*dt;
  const double s    = std::sin(half)/w;
  q = mul(q,Quat{std::cos(half), g[0]*s, g[1]*s, g[2]*s});
  const double qn = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
  q = {q.w/qn, q.x/qn, q.y/qn, q.z/qn};
  }

void HandProcessor::process(const Packet& p, std::vector<Processed>& out) {
  if(p.samples.empty())
    return;
  const bool    hadTime = anyTime;
  const int64_t prevUs  = lastUs;
  std::vector<int64_t> t;
  stamp(p,t);

  int64_t prev = prevUs;
  bool    havePrev = hadTime;
  for(size_t i=0; i<p.samples.size(); ++i) {
    const RawSample& r = p.samples[i];
    const int64_t now = t[i];
    // The filter steps by the sensor's time: Bluetooth delivers a Joy-Con packet up to ~90 ms late without losing
    // its samples (the gyro goes on smoothly across such a pause), so the step between arrivals would turn the
    // delay into a turn that never happened. The frames' times stay as stamp() put them.
    double dt  = 0;
    bool   gap = false;
    if(havePrev) {
      const int64_t step = r.sensorUs - lastSensorUs;
      gap = now - prev > StreamGapUs;
      dt  = double(step>0 && step<=MaxSensorStepUs ? step : std::min(now - prev, MaxSensorStepUs))*1e-6;
      }
    prev         = now;
    lastSensorUs = r.sensorUs;
    havePrev     = true;
    if(gap) {
      window.clear();
      rest    = false;
      hasQuat = false;
      }

    WindowSample ws;
    ws.tUs = now;
    for(int k=0; k<3; ++k) {
      ws.gyro[k]  = r.gyro[k];
      ws.accel[k] = r.accel[k];
      }
    ws.accelOk = std::abs(norm3(ws.accel) - Gravity)<=RestAccel;
    updateRest(ws);

    bool justEstimated = false;
    if(rest && !hasBias) {
      for(int k=0; k<3; ++k)
        gyroBias[k] = windowMeanGyro[k];
      hasBias       = true;
      // at rest gravity is exact: the orientation starts here, yaw 0
      q             = fromGravity(windowMeanAccel);
      hasQuat       = true;
      justEstimated = true;
      }
    else if(rest) {
      const double k = std::min(1.0, dt/BiasTauS);
      for(int a=0; a<3; ++a)
        gyroBias[a] += (windowMeanGyro[a] - gyroBias[a])*k;
      }

    double g[3];
    for(int k=0; k<3; ++k)
      g[k] = ws.gyro[k] - (hasBias ? gyroBias[k] : 0.0);

    if(hasBias && !justEstimated) {
      if(!hasQuat) {
        // after a break in the stream: start again from gravity once the hand is roughly still
        if(std::abs(norm3(ws.accel) - Gravity)<=GravityGate && norm3(g)<0.5) {
          q       = fromGravity(ws.accel);
          hasQuat = true;
          }
        }
      else if(!gap && dt>0) {
        integrate(g,ws.accel,dt);
        }
      }

    Processed s;
    s.tUs     = uint64_t(now);
    s.buttons = r.buttons;
    if(hasBias)
      s.flags |= HandMotion::GyroBiasRemoved;
    if(rest)
      s.flags |= HandMotion::AtRest;
    for(int k=0; k<3; ++k) {
      s.gyro[k]  = float(g[k]);
      s.accel[k] = r.accel[k];
      }
    if(quatValid()) {
      s.flags  |= HandMotion::QuatValid;
      s.quat[0] = float(q.w);
      s.quat[1] = float(q.x);
      s.quat[2] = float(q.y);
      s.quat[3] = float(q.z);
      }
    out.push_back(s);
    }
  }

HandMotion::Frame makeFrame(HandMotion::Hand hand, uint16_t session, uint32_t seq, const Processed& s) {
  HandMotion::Frame f;
  f.hand    = hand;
  f.flags   = s.flags;
  f.session = session;
  f.seq     = seq;
  f.buttons = s.buttons;
  f.tUs     = s.tUs;
  for(int i=0; i<3; ++i) {
    f.gyro[i]  = s.gyro[i];
    f.accel[i] = s.accel[i];
    }
  for(int i=0; i<4; ++i)
    f.quat[i] = s.quat[i];
  return f;
  }

Bridge::Bridge(uint16_t session, FrameSink& sink)
  :sess(session), sink(sink) {
  }

void Bridge::device(HandMotion::Hand hand, const std::string& name) {
  Hand& h = hands[hand];
  if(!h.name.empty() && h.name!=name)
    h.proc.reset();
  h.name = name;
  }

void Bridge::feed(const Packet& p) {
  if(p.hand>=HandMotion::HandCount)
    return;
  Hand& h = hands[p.hand];
  scratch.clear();
  h.proc.process(p,scratch);
  for(auto& s:scratch) {
    sink.send(makeFrame(p.hand,sess,h.seq,s));
    ++h.seq;
    ++h.count;
    }
  }

uint64_t Bridge::takeCount(HandMotion::Hand h) {
  const uint64_t n = hands[h].count;
  hands[h].count = 0;
  return n;
  }

}
