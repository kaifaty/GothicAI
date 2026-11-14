// --self-test: the bridge's checks without devices. The data is tests/excerpt.csv, cut from the Joy-Con runs of
// 2026-11-14 (tests/make_excerpt.py): part 1, both Joy-Con on the table; 2 s of silence; part 2, the right one on
// the table, picked up, three swings, held.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "motion.h"
#include "record.h"
#include "run.h"
#include "sinks.h"

#ifndef HMB_TEST_DATA
#error "HMB_TEST_DATA: the path of tests/excerpt.csv"
#endif

namespace Bridge {

namespace {

constexpr double RadToDeg = 180.0/3.14159265358979323846;

struct Collect final : FrameSink {
  std::vector<HandMotion::Frame> frames;
  void send(const HandMotion::Frame& f) override { frames.push_back(f); }
  };

int failures = 0;

void report(bool ok, const char* name, const std::string& detail) {
  std::printf("%s %-18s %s\n",ok ? "ok  " : "FAIL",name,detail.c_str());
  if(!ok)
    ++failures;
  }

std::string fmt(const char* f, ...) __attribute__((format(printf,1,2)));
std::string fmt(const char* f, ...) {
  char buf[512];
  va_list a;
  va_start(a,f);
  std::vsnprintf(buf,sizeof(buf),f,a);
  va_end(a);
  return buf;
  }

double len3(const float v[3]) {
  return std::sqrt(double(v[0])*v[0] + double(v[1])*v[1] + double(v[2])*v[2]);
  }

double median(std::vector<double> v) {
  if(v.empty())
    return NAN;
  std::nth_element(v.begin(),v.begin()+v.size()/2,v.end());
  return v[v.size()/2];
  }

// the angle between the measured "up" and the one the frame's orientation predicts, degrees
double tiltError(const HandMotion::Frame& f) {
  const double w = f.quat[0], x = f.quat[1], y = f.quat[2], z = f.quat[3];
  const double v[3] = {2*(x*z - w*y), 2*(w*x + y*z), w*w - x*x - y*y + z*z};
  const double n = len3(f.accel);
  double c = (v[0]*f.accel[0] + v[1]*f.accel[1] + v[2]*f.accel[2])/n;
  c = std::clamp(c,-1.0,1.0);
  return std::acos(c)*RadToDeg;
  }

double yawOf(const HandMotion::Frame& f) {
  double roll = 0, pitch = 0, yaw = 0;
  toEuler(Quat{f.quat[0],f.quat[1],f.quat[2],f.quat[3]},roll,pitch,yaw);
  return yaw;
  }

std::vector<const HandMotion::Frame*> ofHand(const std::vector<HandMotion::Frame>& all, HandMotion::Hand h) {
  std::vector<const HandMotion::Frame*> out;
  for(auto& f:all)
    if(f.hand==h)
      out.push_back(&f);
  return out;
  }

// ---------------------------------------------------------------------------------------------------------------

void frameRoundTrip(const std::vector<RecordEvent>& ev) {
  // one hand-made frame with every field away from its default, then the frames of a real packet
  Processed s;
  s.tUs     = 0x0123456789ABull;
  s.flags   = HandMotion::GyroBiasRemoved | HandMotion::QuatValid;
  s.buttons = 0x80000005u;
  const float g[3] = {-12.5f, 0.001f, 3.25f}, a[3] = {9.80665f, -0.5f, -30.0f}, q[4] = {0.5f, -0.5f, 0.5f, -0.5f};
  std::copy(g,g+3,s.gyro);
  std::copy(a,a+3,s.accel);
  std::copy(q,q+4,s.quat);
  std::vector<HandMotion::Frame> made = {makeFrame(HandMotion::Right,0xBEEF,0x89ABCDEFu,s)};

  Collect sink;
  Bridge  b(0x1234,sink);
  for(auto& e:ev)
    if(!e.isDevice) {
      b.feed(e.packet);
      break;
      }
  made.insert(made.end(),sink.frames.begin(),sink.frames.end());

  int bad = 0;
  for(size_t i=0; i<made.size(); ++i) {
    const auto& m = made[i];
    uint8_t buf[HandMotion::FrameSize];
    HandMotion::encode(m,buf);
    HandMotion::Frame d;
    const bool same = HandMotion::decode(buf,sizeof(buf),d)==HandMotion::Status::Ok &&
                      d.hand==m.hand && d.flags==m.flags && d.session==m.session && d.seq==m.seq &&
                      d.buttons==m.buttons && d.tUs==m.tUs &&
                      std::memcmp(d.gyro,m.gyro,sizeof(d.gyro))==0 && std::memcmp(d.accel,m.accel,sizeof(d.accel))==0 &&
                      std::memcmp(d.quat,m.quat,sizeof(d.quat))==0;
    // what the bridge meant to put in: the first frame is the hand-made one, the rest follow the packet
    bool meant = true;
    if(i==0) {
      meant = m.hand==HandMotion::Right && m.session==0xBEEF && m.seq==0x89ABCDEFu && m.tUs==s.tUs &&
              m.flags==s.flags && m.buttons==s.buttons && std::equal(g,g+3,m.gyro) && std::equal(a,a+3,m.accel) &&
              std::equal(q,q+4,m.quat);
      } else {
      meant = m.session==0x1234 && m.seq==uint32_t(i-1);
      }
    if(!same || !meant)
      ++bad;
    }
  report(bad==0,"frame-roundtrip",fmt("%zu frames through encode/decode of handmotionframe.h, %d differ",made.size(),bad));
  }

void splitParts(const std::vector<RecordEvent>& ev, std::vector<RecordEvent>& part1, std::vector<RecordEvent>& part2) {
  int64_t prev = 0;
  bool    any = false, second = false;
  for(auto& e:ev) {
    if(!e.isDevice) {
      if(any && e.packet.arrivalUs - prev>1'000'000)
        second = true;
      prev = e.packet.arrivalUs;
      any  = true;
      }
    (second ? part2 : part1).push_back(e);
    }
  }

std::vector<HandMotion::Frame> feedAll(const std::vector<RecordEvent>& ev, Bridge& b, Collect& sink) {
  for(auto& e:ev) {
    if(e.isDevice)
      b.device(e.hand,e.device); else
      b.feed(e.packet);
    }
  return sink.frames;
  }

void biasLeft(const std::vector<RecordEvent>& part1) {
  Collect sink;
  Bridge  b(1,sink);
  const auto frames = feedAll(part1,b,sink);
  std::vector<double> raw, after;
  for(auto& e:part1)
    if(!e.isDevice && e.hand==HandMotion::Left)
      for(auto& s:e.packet.samples)
        raw.push_back(len3(s.gyro)*RadToDeg);
  uint64_t t0 = 0, firstEst = 0;
  for(auto* f:ofHand(frames,HandMotion::Left)) {
    if(t0==0)
      t0 = f->tUs;
    if(f->flags & HandMotion::GyroBiasRemoved) {
      if(firstEst==0)
        firstEst = f->tUs;
      after.push_back(len3(f->gyro)*RadToDeg);
      }
    }
  const double r = median(raw), m = median(after);
  report(after.size()>=200 && m<0.3 && r>3,"bias-left-rest",
         fmt("raw median |w| %.2f deg/s; bias set %.2f s after the first sample; then median |w| %.3f deg/s over %zu "
             "samples (limit 0.3)",r,double(firstEst-t0)*1e-6,m,after.size()));
  }

void tilt(const std::vector<RecordEvent>& part1, const std::vector<RecordEvent>& part2) {
  // at rest: both hands of part 1 and the right one on the table in part 2
  Collect sink;
  Bridge  b(1,sink);
  feedAll(part1,b,sink);
  const size_t part2From = sink.frames.size();
  const auto   frames    = feedAll(part2,b,sink);
  double worstRest = 0;
  size_t nRest = 0;
  for(auto& f:frames)
    if((f.flags & HandMotion::QuatValid) && (f.flags & HandMotion::AtRest)) {
      worstRest = std::max(worstRest,tiltError(f));
      ++nRest;
      }
  report(nRest>=500 && worstRest<=2.0,"tilt-at-rest",
         fmt("%zu frames at rest, the worst tilt error %.2f deg (limit 2)",nRest,worstRest));

  // part 2, right hand: the swings, then the last second, when the hand holds still again
  std::vector<const HandMotion::Frame*> right;
  for(size_t i=part2From; i<frames.size(); ++i)
    if(frames[i].hand==HandMotion::Right)
      right.push_back(&frames[i]);
  int    swings = 0;
  bool   inside = false;
  double peak = 0;
  for(auto* f:right) {
    const double w = len3(f->gyro)*RadToDeg;
    peak = std::max(peak,w);
    if(w>300 && !inside) {
      ++swings;
      inside = true;
      }
    else if(w<60) {
      inside = false;
      }
    }
  std::vector<double> err;
  double still = 0;
  const uint64_t end = right.empty() ? 0 : right.back()->tUs;
  for(auto* f:right)
    if(f->tUs + 1'000'000>=end && (f->flags & HandMotion::QuatValid)) {
      err.push_back(tiltError(*f));
      still = std::max(still,len3(f->gyro)*RadToDeg);
      }
  const double m = median(err);
  report(swings>=3 && err.size()>=100 && m<=5.0,"tilt-after-swings",
         fmt("%d swings over 300 deg/s, peak %.0f deg/s; held still afterwards (|w| up to %.0f deg/s): median "
             "tilt error %.2f deg over the last second (limit 5)",swings,peak,still,m));
  }

void yawDrift(const std::vector<RecordEvent>& part1) {
  // 30 s at rest: part 1 played in a loop, every pass right after the previous one, as --loop does
  std::vector<RecordEvent> loop;
  int64_t first = 0, last = 0;
  for(auto& e:part1)
    if(!e.isDevice) {
      if(first==0)
        first = e.packet.arrivalUs;
      last = e.packet.arrivalUs;
      }
  const int64_t pass = last - first + 15'000;
  const int     passes = int(32'000'000/pass) + 2;
  for(int k=0; k<passes; ++k)
    for(auto e:part1) {
      if(e.isDevice && k>0)
        continue;
      e.packet.arrivalUs += int64_t(k)*pass;
      loop.push_back(std::move(e));
      }
  Collect sink;
  Bridge  b(1,sink);
  const auto frames = feedAll(loop,b,sink);
  bool        ok = true;
  std::string detail;
  for(auto h:{HandMotion::Left, HandMotion::Right}) {
    double   y0 = 0, worst = 0;
    uint64_t t0 = 0, tEnd = 0;
    for(auto* f:ofHand(frames,h)) {
      if(!(f->flags & HandMotion::QuatValid))
        continue;
      if(t0==0) {
        t0 = f->tUs;
        y0 = yawOf(*f);
        }
      if(f->tUs>t0 + 30'000'000)
        break;
      double d = yawOf(*f) - y0;
      d = std::remainder(d,360.0);
      worst = std::max(worst,std::abs(d));
      tEnd  = f->tUs;
      }
    const double span = double(tEnd - t0)*1e-6;
    ok = ok && t0!=0 && span>=29.9 && worst<=2.0;
    detail += fmt("%s: %.1f s, yaw drifts up to %.2f deg; ",h==HandMotion::Left ? "left" : "right",span,worst);
    }
  report(ok,"yaw-rest-30s",detail+fmt("a %.1f s stretch of the table played %d times (limit 2 deg)",
                                       double(pass)*1e-6,passes));
  }

void timing(const std::vector<RecordEvent>& ev) {
  Collect sink;
  Bridge  b(1,sink);
  feedAll(ev,b,sink);
  const auto& frames = sink.frames;
  size_t  next[HandMotion::HandCount] = {};
  auto    byHand = std::vector<std::vector<const HandMotion::Frame*>>{ofHand(frames,HandMotion::Left),
                                                                     ofHand(frames,HandMotion::Right)};
  size_t  packets = 0, squeezed = 0, wrongSqueeze = 0, notGrowing = 0;
  double  worstStep = 0;
  for(auto& h:byHand)
    for(size_t i=1; i<h.size(); ++i)
      if(h[i]->tUs<=h[i-1]->tUs)
        ++notGrowing;
  for(auto& e:ev) {
    if(e.isDevice)
      continue;
    const auto& p  = e.packet;
    auto&       fr = byHand[p.hand];
    const size_t at = next[p.hand];
    next[p.hand] += p.samples.size();
    ++packets;
    bool fits = fr[at+p.samples.size()-1]->tUs==uint64_t(p.arrivalUs);
    double worst = 0;
    for(size_t i=1; i<p.samples.size(); ++i) {
      const double d = double(int64_t(fr[at+i]->tUs - fr[at+i-1]->tUs) - (p.samples[i].sensorUs - p.samples[i-1].sensorUs));
      worst = std::max(worst,std::abs(d));
      }
    if(worst>500)
      fits = false;
    if(fits) {
      worstStep = std::max(worstStep,worst);
      continue;
      }
    ++squeezed;
    // squeezing is allowed only where the sensor steps would have gone back past the hand's previous sample
    const int64_t wouldStart = p.arrivalUs - (p.samples.back().sensorUs - p.samples.front().sensorUs);
    if(at==0 || wouldStart>int64_t(fr[at-1]->tUs))
      ++wrongSqueeze;
    }
  const uint64_t reported = b.processor(HandMotion::Left).squeezed() + b.processor(HandMotion::Right).squeezed();
  report(notGrowing==0 && wrongSqueeze==0 && squeezed==reported && squeezed*100<=packets*3,"time",
         fmt("%zu frames grow strictly (%zu do not); %zu packets: in-packet step = sensor step within %.0f us "
             "(limit 500); %zu packets squeezed because they overlapped the previous one (%.1f%%, %zu without "
             "reason)",frames.size(),notGrowing,packets,worstStep,squeezed,100.0*double(squeezed)/double(packets),
             wrongSqueeze));
  }

void loopback(const std::vector<RecordEvent>& ev) {
  const int rx = ::socket(AF_INET,SOCK_DGRAM,0);
  sockaddr_in a = {};
  a.sin_family      = AF_INET;
  a.sin_port        = 0;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  const int big = 4*1024*1024;
  ::setsockopt(rx,SOL_SOCKET,SO_RCVBUF,&big,sizeof(big));
  timeval tv = {0,100'000};
  ::setsockopt(rx,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  if(rx<0 || ::bind(rx,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0 ||
     ::getsockname(rx,reinterpret_cast<sockaddr*>(&a),&alen)!=0) {
    report(false,"loopback",fmt("cannot open the test socket: %s",std::strerror(errno)));
    if(rx>=0)
      ::close(rx);
    return;
    }
  const uint16_t port = ntohs(a.sin_port);

  std::vector<HandMotion::Frame> got;
  size_t            bad = 0;
  std::atomic<bool> done{false};
  std::thread reader([&]() {
    uint8_t buf[256];
    for(;;) {
      const ssize_t n = ::recv(rx,buf,sizeof(buf),0);
      if(n<0) {
        if(done.load())
          return;
        continue;
        }
      HandMotion::Frame f;
      if(HandMotion::decode(buf,size_t(n),f)==HandMotion::Status::Ok)
        got.push_back(f); else
        ++bad;
      }
    });

  UdpSender   tx;
  std::string err;
  size_t      samples = 0;
  for(auto& e:ev)
    if(!e.isDevice)
      samples += e.packet.samples.size();
  uint16_t session = 0;
  if(tx.open("127.0.0.1",port,err)) {
    Bridge        b(0xA11C,tx);
    ReplayOptions o;
    o.speed = 20;
    replay(ev,b,o);
    session = b.session();
    }
  done.store(true);
  reader.join();
  ::close(rx);

  uint32_t want[HandMotion::HandCount] = {};
  size_t   gaps = 0, foreign = 0;
  for(auto& f:got) {
    if(f.seq!=want[f.hand])
      ++gaps;
    want[f.hand] = f.seq+1;
    if(f.session!=session)
      ++foreign;
    }
  report(err.empty() && got.size()==samples && gaps==0 && bad==0 && foreign==0 && tx.errors()==0,"loopback",
         fmt("port %u: %zu of %zu frames came (%zu bad, %llu send errors%s%s); seq in a row: left 0..%d, right 0..%d, "
             "%zu breaks; session %04x on all",unsigned(port),got.size(),samples,bad,(unsigned long long)tx.errors(),
             err.empty() ? "" : ", ",err.c_str(),int(want[0])-1,int(want[1])-1,gaps,unsigned(session)));
  }

void recordRoundTrip(const std::vector<RecordEvent>& ev) {
  char path[] = "/tmp/hand-motion-bridge-XXXXXX";
  const int fd = ::mkstemp(path);
  if(fd<0) {
    report(false,"record","mkstemp failed");
    return;
    }
  ::close(fd);
  std::string err;
  {
    RecordWriter w;
    if(w.open(path,err))
      for(auto& e:ev) {
        if(e.isDevice)
          w.device(e.hand,e.device); else
          w.packet(e.packet);
        }
  }
  std::vector<RecordEvent> back;
  const bool read = err.empty() && readRecord(path,back,err);
  ::unlink(path);
  size_t diff = back.size()==ev.size() ? 0 : 1;
  for(size_t i=0; read && diff==0 && i<ev.size(); ++i) {
    const auto& x = ev[i];
    const auto& y = back[i];
    bool same = x.isDevice==y.isDevice && x.hand==y.hand && x.device==y.device &&
                x.packet.arrivalUs==y.packet.arrivalUs && x.packet.samples.size()==y.packet.samples.size();
    for(size_t k=0; same && k<x.packet.samples.size(); ++k) {
      const auto& s = x.packet.samples[k];
      const auto& t = y.packet.samples[k];
      same = s.sensorUs==t.sensorUs && s.buttons==t.buttons && std::equal(s.gyro,s.gyro+3,t.gyro) &&
             std::equal(s.accel,s.accel+3,t.accel);
      }
    if(!same)
      ++diff;
    }
  report(read && diff==0,"record",fmt("the excerpt written by --record's writer and read back: %zu events, %zu "
                                      "differ%s%s",back.size(),diff,err.empty() ? "" : "; ",err.c_str()));
  }

}

int runSelfTest() {
  std::vector<RecordEvent> ev;
  std::string err;
  if(!readRecord(HMB_TEST_DATA,ev,err)) {
    std::printf("FAIL cannot read the excerpt: %s\n",err.c_str());
    return 1;
    }
  std::vector<RecordEvent> part1, part2;
  splitParts(ev,part1,part2);

  frameRoundTrip(ev);
  biasLeft(part1);
  tilt(part1,part2);
  yawDrift(part1);
  timing(ev);
  loopback(ev);
  recordRoundTrip(ev);

  std::printf("%s: %d failed\n",failures==0 ? "self-test passed" : "self-test FAILED",failures);
  return failures==0 ? 0 : 1;
  }

}
