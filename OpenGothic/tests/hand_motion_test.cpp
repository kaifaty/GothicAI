// gothic-hand-motion-test: the hand motion frame and its receiver (docs/hand-motion.md), without the game.
#include "input/handmotion.h"

#include <Tempest/Log>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
using Receiver = HandMotionReceiver;

namespace {
size_t assertions = 0;

void require(bool value, const std::string& message) {
  ++assertions;
  if(!value)
    throw std::runtime_error(message);
  }

Receiver::Config offline() {
  Receiver::Config c;
  c.enabled = false;
  return c;
  }

Receiver::Config anyPort() {
  Receiver::Config c;
  c.port = 0;
  return c;
  }

HandMotion::Frame frame(uint8_t hand, uint32_t seq, uint64_t tUs = 0, uint16_t session = 0) {
  HandMotion::Frame f;
  f.hand    = hand;
  f.seq     = seq;
  f.tUs     = tUs;
  f.session = session;
  f.flags   = uint8_t(seq&7);
  f.gyro[0] = float(seq)*0.5f;
  return f;
  }

std::vector<uint8_t> bytes(const HandMotion::Frame& f) {
  std::vector<uint8_t> b(HandMotion::FrameSize);
  HandMotion::encode(f,b.data());
  return b;
  }

bool feed(Receiver& r, const HandMotion::Frame& f, Receiver::Clock::time_point at) {
  const auto b = bytes(f);
  return r.accept(b.data(),b.size(),at);
  }

struct Sender {
  int         fd = -1;
  sockaddr_in to = {};

  explicit Sender(uint16_t port) {
    fd = ::socket(AF_INET,SOCK_DGRAM,0);
    require(fd>=0,"sender socket");
    to.sin_family      = AF_INET;
    to.sin_port        = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
  ~Sender() { ::close(fd); }

  void send(const void* data, size_t size) {
    const auto n = ::sendto(fd,data,size,0,reinterpret_cast<const sockaddr*>(&to),sizeof(to));
    require(n==ssize_t(size),"sendto");
    }
  void send(const HandMotion::Frame& f) {
    const auto b = bytes(f);
    send(b.data(),b.size());
    }
  };

// let the loopback deliver what was sent before the next tick
void settle() {
  std::this_thread::sleep_for(milliseconds(50));
  }

// encode -> decode keeps every field; bad magic, length and hand are dropped and counted
void encodeDecode() {
  HandMotion::Frame f;
  f.hand     = HandMotion::Right;
  f.flags    = uint8_t(HandMotion::GyroBiasRemoved | HandMotion::AtRest | HandMotion::QuatValid);
  f.session  = 0xBEEF;
  f.seq      = 0xA1B2C3D4u;
  f.buttons  = (1u<<9) | (1u<<0);
  f.tUs      = 0x0102030405060708ull;
  f.gyro[0]  = -1.25f; f.gyro[1]  = 3.5f;  f.gyro[2]  = 1e-3f;
  f.accel[0] = 0.f;    f.accel[1] = -9.81f; f.accel[2] = 12.f;
  f.quat[0]  = 0.5f;   f.quat[1]  = -0.5f; f.quat[2]  = 0.5f; f.quat[3] = -0.5f;
  const auto b = bytes(f);
  require(b[0]=='G' && b[3]=='1' && b[4]==1 && b[6]==0xEF && b[7]==0xBE && b[8]==0xD4 && b[11]==0xA1 && b[16]==0x08 && b[23]==0x01,
          "little-endian bytes at the table's offsets");

  HandMotion::Frame g;
  require(HandMotion::decode(b.data(),b.size(),g)==HandMotion::Status::Ok,"decode a good frame");
  require(std::equal(f.magic,f.magic+4,g.magic) && g.hand==f.hand && g.flags==f.flags && g.session==f.session &&
          g.seq==f.seq && g.buttons==f.buttons && g.tUs==f.tUs,"integer fields survive");
  for(int i=0; i<3; ++i)
    require(g.gyro[i]==f.gyro[i] && g.accel[i]==f.accel[i],"gyro and accel survive");
  for(int i=0; i<4; ++i)
    require(g.quat[i]==f.quat[i],"quat survives");
  for(uint8_t flags:{uint8_t(0),uint8_t(HandMotion::GyroBiasRemoved|HandMotion::QuatValid),uint8_t(HandMotion::AtRest)}) {
    auto h = f;
    h.flags = flags;
    const auto hb = bytes(h);
    require(HandMotion::decode(hb.data(),hb.size(),g)==HandMotion::Status::Ok && g.flags==flags,
            "flags "+std::to_string(flags)+" survive, off bits stay off");
    }

  Receiver r(offline());
  const auto now = Receiver::Clock::now();
  for(size_t i=0; i<4; ++i) {
    auto magic = b;
    magic[i] = 'X';
    require(HandMotion::decode(magic.data(),magic.size(),g)==HandMotion::Status::BadMagic,"bad magic byte "+std::to_string(i));
    require(!r.accept(magic.data(),magic.size(),now),"receiver drops bad magic byte "+std::to_string(i));
    }
  for(uint8_t h:{uint8_t(2),uint8_t(255)}) {
    auto hand = b;
    hand[4] = h;
    require(HandMotion::decode(hand.data(),hand.size(),g)==HandMotion::Status::BadHand,"hand "+std::to_string(h));
    require(!r.accept(hand.data(),hand.size(),now),"receiver drops hand "+std::to_string(h));
    }
  require(HandMotion::decode(b.data(),63,g)==HandMotion::Status::BadSize,"63 bytes");
  auto longer = b;
  longer.push_back(0);
  require(!r.accept(b.data(),63,now),"receiver drops 63 bytes");
  require(!r.accept(longer.data(),longer.size(),now),"receiver drops 65 bytes");
  require(r.malformed()==8,"eight bad datagrams counted, got "+std::to_string(r.malformed()));
  require(r.hand(HandMotion::Left).last()==nullptr && r.hand(HandMotion::Right).last()==nullptr,"no hand got a frame");
  require(r.accept(b.data(),b.size(),now) && r.hand(HandMotion::Right).stats().frames==1,"the good one is accepted");
  }

// frames of two hands through a real socket come apart by hand and in seq order
void socketTwoHands() {
  Receiver r(anyPort());
  require(r.isOpen() && r.port()!=0,"open on a free port: "+r.error());
  Sender s(r.port());
  for(uint32_t i=0; i<20; ++i) {
    s.send(frame(HandMotion::Left, 1+i));
    s.send(frame(HandMotion::Right,101+i));
    }
  // a good frame with a byte more or less: recv must not cut or pass it
  auto longer = bytes(frame(HandMotion::Left,50));
  longer.push_back(0);
  s.send(longer.data(),longer.size());
  s.send(longer.data(),HandMotion::FrameSize-1);
  settle();
  r.tick();
  for(uint8_t h=0; h<HandMotion::HandCount; ++h) {
    const auto& t = r.hand(HandMotion::Hand(h));
    require(t.size()==20 && t.stats().frames==20 && t.stats().lost==0,"20 frames of hand "+std::to_string(h));
    const uint32_t first = h==HandMotion::Left ? 1 : 101;
    for(size_t i=0; i<t.size(); ++i) {
      require(t[i].frame.hand==h && t[i].frame.seq==first+i,"hand and seq order");
      require(t[i].frame.gyro[0]==float(first+i)*0.5f && t[i].frame.flags==((first+i)&7),"payload and flags");
      }
    require(t.last()!=nullptr && t.last()->frame.seq==first+19,"last frame");
    }
  require(r.malformed()==2,"65 and 63 bytes counted as malformed, got "+std::to_string(r.malformed()));
  }

// a gap 10 -> 13 is 2 lost; a repeat and a lower seq are dropped; a pause of 1.1 s changes none of it
void seqGaps() {
  Receiver r(offline());
  const auto t0 = Receiver::Clock::now();
  auto at = t0;
  require(feed(r,frame(HandMotion::Left,10,1'000'000),at),"seq 10");
  require(feed(r,frame(HandMotion::Left,13,1'005'000),at+=milliseconds(1100)),"seq 13");
  require(r.hand(HandMotion::Left).stats().lost==2,"10 -> 13: 2 lost");
  const auto late = duration<double,std::milli>(r.hand(HandMotion::Left).last()->time-t0).count();
  require(std::abs(late-5.0)<0.01,"13 keeps the clock of 10: 5 ms after it, got "+std::to_string(late));
  require(!feed(r,frame(HandMotion::Left,13),at+=milliseconds(1100)),"repeat dropped");
  require(!feed(r,frame(HandMotion::Left,10),at+=milliseconds(1100)),"lower dropped");
  const auto& st = r.hand(HandMotion::Left).stats();
  require(st.dropped==2 && st.frames==2 && st.lost==2,"dropped 2, frames 2, lost still 2");
  require(r.hand(HandMotion::Left).size()==1 && r.hand(HandMotion::Left)[0].frame.seq==13,"history: 10 aged, 13 stays");
  require(feed(r,frame(HandMotion::Left,14),at+=milliseconds(5)) && st.lost==2,"14 follows 13 without a loss");
  require(feed(r,frame(HandMotion::Left,17),at+=milliseconds(5)) && st.lost==4,"14 -> 17: the losses add up to 4");
  require(r.hand(HandMotion::Right).stats().frames==0,"the other hand is untouched");
  }

// a new session is a new stream: seq from 0 is taken at once, no loss, the clock starts over; within a session seq
// back is still dropped
void sessionRestart() {
  Receiver r(offline());
  const auto     t0  = Receiver::Clock::now();
  const uint64_t was = 9'000'000'000'000ull; // the old sender's clock
  const uint64_t now = 1'000'000ull;         // the new one started later from a smaller origin
  for(int k=0; k<50; ++k)
    require(feed(r,frame(HandMotion::Right,uint32_t(100+k),was+uint64_t(5000*k),7),t0+microseconds(5000*k+500)),
            "old stream "+std::to_string(k));
  require(!feed(r,frame(HandMotion::Right,120,was,7),t0+milliseconds(260)),"same session, seq back: dropped");
  const auto& t = r.hand(HandMotion::Right);
  for(int k=0; k<50; ++k) {
    const auto sent = t0+milliseconds(300)+microseconds(5000*k);
    require(feed(r,frame(HandMotion::Right,uint32_t(k),now+uint64_t(5000*k),8),sent+microseconds(700)),
            "new stream "+std::to_string(k)+" accepted at once");
    const auto err = duration<double,std::milli>(t.last()->time-sent).count();
    require(std::abs(err)<=2.0,"new stream "+std::to_string(k)+" off by "+std::to_string(err)+" ms");
    }
  require(t.stats().restarts==1 && t.stats().lost==0 && t.stats().dropped==1 && t.stats().frames==100,
          "restarts 1, lost 0, dropped 1, frames 100; got restarts "+std::to_string(t.stats().restarts)+", lost "+
          std::to_string(t.stats().lost));
  require(t.size()==50 && t[0].frame.seq==0 && t[0].frame.session==8,"the old stream's history is gone");
  require(!feed(r,frame(HandMotion::Right,10,now,8),t0+milliseconds(600)),"new session, seq back: dropped");
  require(t.stats().dropped==2 && t.stats().restarts==1,"dropped 2, still one restart");
  require(feed(r,frame(HandMotion::Right,0,now,0),t0+milliseconds(700)),"session 8 -> 0, seq 0: a new stream as well");
  require(t.stats().restarts==2 && t.stats().lost==0 && t.size()==1,"zero has no special meaning: restarts 2");
  require(r.hand(HandMotion::Left).stats().restarts==0,"the other hand is untouched");
  }

// tick on an empty socket changes nothing and does not block
void emptyTick() {
  Receiver r(anyPort());
  require(r.isOpen(),"open: "+r.error());
  Sender s(r.port());
  for(uint32_t seq:{10u,13u,14u,15u,16u,16u})
    s.send(frame(HandMotion::Right,seq));
  settle();
  r.tick();
  const auto& t = r.hand(HandMotion::Right);
  require(t.size()==5 && t.stats().frames==5 && t.stats().lost==2 && t.stats().dropped==1,
          "5 frames, 2 lost and 1 dropped before the empty ticks");
  const auto before = t.stats();
  const auto lastAt = t.lastReceived();
  for(int i=0; i<21; ++i) {
    const auto t0 = steady_clock::now();
    r.tick();
    const auto ms = duration<double,std::milli>(steady_clock::now()-t0).count();
    require(ms<1.0,"tick "+std::to_string(i)+" under 1 ms, got "+std::to_string(ms));
    }
  require(t.size()==5 && t[0].frame.seq==10 && t.last()->frame.seq==16 && t.lastReceived()==lastAt,"history kept");
  require(t.stats().lost==2 && t.stats().dropped==1,"lost 2 and dropped 1 kept, got "+std::to_string(t.stats().lost)+
          ", "+std::to_string(t.stats().dropped));
  require(t.stats().frames==before.frames && t.stats().lost==before.lost && t.stats().dropped==before.dropped &&
          t.stats().overflow==before.overflow && t.stats().restarts==before.restarts,"statistics kept");
  require(r.hand(HandMotion::Left).size()==0 && r.malformed()==0 && r.isOpen(),"still open, nothing counted");
  }

// more frames than the history holds: the newest stay, the rest count as overflow
void historyOverflow() {
  Receiver r(offline());
  const uint32_t total = uint32_t(Receiver::HistoryCapacity)+88;
  auto at = Receiver::Clock::now();
  for(uint32_t seq=1; seq<=total; ++seq)
    feed(r,frame(HandMotion::Right,seq),at+=microseconds(1000));
  const auto& t = r.hand(HandMotion::Right);
  require(t.size()==Receiver::HistoryCapacity,"history is full");
  require(t[0].frame.seq==total-Receiver::HistoryCapacity+1 && t[t.size()-1].frame.seq==total,"the newest by seq stay");
  for(size_t i=1; i<t.size(); ++i)
    require(t[i].frame.seq==t[i-1].frame.seq+1,"in seq order");
  require(t.stats().overflow==88 && t.stats().frames==total && t.stats().lost==0,
          "88 overflow, got "+std::to_string(t.stats().overflow));
  require(t.last()!=nullptr && t.last()->frame.seq==total,"last() is the last frame sent");

  // a pause of 3 s: what aged leaves first, 400 newer frames fit without an overflow
  Receiver p(offline());
  const auto t0 = Receiver::Clock::now();
  for(uint32_t seq=1; seq<=200; ++seq)
    feed(p,frame(HandMotion::Left,seq),t0+milliseconds(5*seq));
  for(uint32_t seq=201; seq<=600; ++seq)
    feed(p,frame(HandMotion::Left,seq),t0+milliseconds(3000));
  const auto& q = p.hand(HandMotion::Left);
  require(q.size()==400 && q[0].frame.seq==201 && q.stats().overflow==0 && q.stats().lost==0,
          "after a pause: 400 frames, 0 overflow, got "+std::to_string(q.size())+", "+std::to_string(q.stats().overflow));
  }

// the socket asks for >= 256 KB; 400 frames sent between two ticks all come
void receiveBuffer() {
  Receiver r(anyPort());
  require(r.isOpen(),"open: "+r.error());
  // asked for >= 256 KB: on Linux getsockopt shows twice the request
  require(r.receiveBuffer()>=Receiver::ReceiveBufferReported,"SO_RCVBUF granted >= 256 KB, got "+
          std::to_string(r.receiveBuffer())+" of "+std::to_string(Receiver::ReceiveBufferReported)+" reported");
  r.tick();
  Sender s(r.port());
  for(uint32_t i=0; i<200; ++i) {
    s.send(frame(HandMotion::Left, 1+i));
    s.send(frame(HandMotion::Right,1+i));
    }
  settle();
  r.tick();
  const auto& l = r.hand(HandMotion::Left).stats();
  const auto& rt = r.hand(HandMotion::Right).stats();
  require(l.frames+rt.frames==400,"400 frames, got "+std::to_string(l.frames+rt.frames));
  require(l.lost==0 && rt.lost==0 && l.overflow==0 && rt.overflow==0,"no loss, no overflow");
  }

// the sender's clock on another origin, one frame 80 ms late: the rest translate within 2 ms, the late one too
void clockOffset() {
  Receiver r(offline());
  const auto     t0     = Receiver::Clock::now();
  const uint64_t origin = 7'000'000'000'000ull; // another origin, far from the game's
  const int      late   = 100;
  int checked = 0;
  for(int k=0; k<300; ++k) {
    if(k>late && k<=late+16)
      continue; // lost while the link stalled: the late frame comes before the next one
    const auto sent     = t0+microseconds(5000*k);
    const auto jitter   = microseconds(300+(k*37%16)*100); // 0.3 .. 1.8 ms on the way
    const auto received = sent+jitter+(k==late ? milliseconds(80) : milliseconds(0));
    require(feed(r,frame(HandMotion::Right,uint32_t(1+k),origin+uint64_t(5000*k)),received),"accepted "+std::to_string(k));
    const auto* s   = r.hand(HandMotion::Right).last();
    const auto  err = duration<double,std::milli>(s->time-sent).count();
    require(std::abs(err)<=2.0,"frame "+std::to_string(k)+" off by "+std::to_string(err)+" ms");
    if(k==late)
      require(duration<double,std::milli>(s->received-s->time).count()>=78.0,"the late frame stays late by receipt");
    ++checked;
    }
  require(checked==300-16,"all checked");
  require(r.hand(HandMotion::Right).stats().lost==16,"the stall's frames are lost");
  Receiver::Clock::time_point back;
  require(r.hand(HandMotion::Right).toGameTime(origin+5000*400,back),"translate a later time");
  const auto err = duration<double,std::milli>(back-(t0+microseconds(5000*400))).count();
  require(std::abs(err)<=2.0,"a later time off by "+std::to_string(err)+" ms");

  // the window: the first frame came in 1 ms, the rest in 11 ms; its minimum holds 2 s, then expires
  for(int k=0; k<=50; ++k) {
    const auto sent = t0+milliseconds(50*k);
    feed(r,frame(HandMotion::Left,uint32_t(1+k),origin+uint64_t(50'000*k)),sent+milliseconds(k==0 ? 1 : 11));
    const auto lag  = duration<double,std::milli>(r.hand(HandMotion::Left).last()->time-sent).count();
    const double want = k<40 ? 1.0 : 11.0; // k=39 came 1.95 s after the first, k=40 2.01 s
    require(std::abs(lag-want)<0.01,"window: frame "+std::to_string(k)+" lags "+std::to_string(lag)+" ms, want "+
            std::to_string(want));
    }
  // a new minimum wins at once: the first frame came in 10 ms, the next in 1 ms
  Receiver q(offline());
  feed(q,frame(HandMotion::Right,1,origin),t0+milliseconds(10));
  feed(q,frame(HandMotion::Right,2,origin+5000),t0+milliseconds(6));
  const auto lag = duration<double,std::milli>(q.hand(HandMotion::Right).last()->time-(t0+milliseconds(5))).count();
  Receiver::Clock::time_point first;
  require(q.hand(HandMotion::Right).toGameTime(origin,first),"translate the first frame");
  const auto firstLag = duration<double,std::milli>(first-t0).count();
  require(std::abs(lag-1.0)<0.01 && std::abs(firstLag-1.0)<0.01,"the faster frame sets the scale at once: lags "+
          std::to_string(lag)+" and "+std::to_string(firstLag)+" ms, want 1");
  }

// a second receiver on a busy port: no crash, an error, tick works
void portInUse() {
  Receiver first(anyPort());
  require(first.isOpen(),"first open: "+first.error());
  Receiver::Config c;
  c.port = first.port();
  struct Capture {
    std::vector<std::string> lines;
    Capture()  { Tempest::Log::setOutputCallback([this](Tempest::Log::Mode,const char* text){ lines.emplace_back(text); }); }
    ~Capture() { Tempest::Log::setOutputCallback(nullptr); }
    } log;
  Receiver second(c);
  require(!second.isOpen() && !second.error().empty() && second.port()==0,"second reports the busy port");
  for(int i=0; i<5; ++i)
    second.tick();
  const auto off = std::count_if(log.lines.begin(),log.lines.end(),[](const std::string& l){
    return l.rfind("[hand-motion] off: bind",0)==0;
    });
  require(off==1,"one log line about the busy port, got "+std::to_string(off));
  Sender s(first.port());
  s.send(frame(HandMotion::Left,1));
  settle();
  first.tick();
  second.tick();
  require(first.hand(HandMotion::Left).stats().frames==1,"the first still receives");
  require(second.hand(HandMotion::Left).stats().frames==0,"the second has nothing");
  }

// OPENGOTHIC_HAND_MOTION=0: the receiver opens no socket
void disabledByEnv() {
  uint16_t port = 0;
  {
    Receiver probe(anyPort());
    require(probe.isOpen(),"probe open: "+probe.error());
    port = probe.port();
  }
  ::unsetenv("OPENGOTHIC_HAND_MOTION");
  ::unsetenv("OPENGOTHIC_HAND_MOTION_PORT");
  ::unsetenv("OPENGOTHIC_HAND_MOTION_LOG");
  const auto def = Receiver::Config::fromEnvironment();
  require(def.enabled && def.port==HandMotion::DefaultPort && !def.log,"on by default, port 47821, no log");

  ::setenv("OPENGOTHIC_HAND_MOTION","0",1);
  ::setenv("OPENGOTHIC_HAND_MOTION_PORT",std::to_string(port).c_str(),1);
  const auto cfg = Receiver::Config::fromEnvironment();
  require(!cfg.enabled && cfg.port==port,"OPENGOTHIC_HAND_MOTION=0 turns it off");

  Receiver off(cfg);
  off.tick();
  ::unsetenv("OPENGOTHIC_HAND_MOTION");
  ::unsetenv("OPENGOTHIC_HAND_MOTION_PORT");
  require(!off.isOpen() && off.port()==0 && off.error().empty(),"off: no socket, no error");
  Receiver::Config again;
  again.port = port;
  Receiver taker(again);
  require(taker.isOpen() && taker.port()==port,"the port stayed free: "+taker.error());
  }
}

int main() {
  const struct { const char* name; void (*run)(); } cases[] = {
    {"encode-decode",    encodeDecode},
    {"socket-two-hands", socketTwoHands},
    {"seq-gaps",         seqGaps},
    {"session-restart",  sessionRestart},
    {"empty-tick",       emptyTick},
    {"history-overflow", historyOverflow},
    {"receive-buffer",   receiveBuffer},
    {"clock-offset",     clockOffset},
    {"port-in-use",      portInUse},
    {"disabled-by-env",  disabledByEnv},
    };
  int failed = 0;
  for(const auto& c:cases) {
    try {
      c.run();
      std::cout << "ok   " << c.name << std::endl;
      }
    catch(const std::exception& e) {
      ++failed;
      std::cout << "FAIL " << c.name << ": " << e.what() << std::endl;
      }
    }
  std::cout << (failed==0 ? "hand motion: ok, " : "hand motion: failed, ") << assertions << " checks, "
            << failed << " of " << std::size(cases) << " cases failed" << std::endl;
  return failed==0 ? 0 : 1;
  }
