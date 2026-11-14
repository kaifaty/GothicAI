// gothic-hand-motion-test: the hand motion frame and its receiver (docs/hand-motion.md), without the game.
#include "input/handmotion.h"

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

HandMotion::Frame frame(uint8_t hand, uint32_t seq, uint64_t tUs = 0) {
  HandMotion::Frame f;
  f.hand    = hand;
  f.seq     = seq;
  f.tUs     = tUs;
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
  f.flags    = uint8_t(HandMotion::GyroBiasRemoved | HandMotion::QuatValid);
  f.reserved = 0;
  f.seq      = 0xA1B2C3D4u;
  f.buttons  = (1u<<9) | (1u<<0);
  f.tUs      = 0x0102030405060708ull;
  f.gyro[0]  = -1.25f; f.gyro[1]  = 3.5f;  f.gyro[2]  = 1e-3f;
  f.accel[0] = 0.f;    f.accel[1] = -9.81f; f.accel[2] = 12.f;
  f.quat[0]  = 0.5f;   f.quat[1]  = -0.5f; f.quat[2]  = 0.5f; f.quat[3] = -0.5f;
  const auto b = bytes(f);
  require(b[0]=='G' && b[3]=='1' && b[4]==1 && b[8]==0xD4 && b[11]==0xA1 && b[16]==0x08 && b[23]==0x01,
          "little-endian bytes at the table's offsets");

  HandMotion::Frame g;
  require(HandMotion::decode(b.data(),b.size(),g)==HandMotion::Status::Ok,"decode a good frame");
  require(std::equal(f.magic,f.magic+4,g.magic) && g.hand==f.hand && g.flags==f.flags && g.reserved==f.reserved &&
          g.seq==f.seq && g.buttons==f.buttons && g.tUs==f.tUs,"integer fields survive");
  for(int i=0; i<3; ++i)
    require(g.gyro[i]==f.gyro[i] && g.accel[i]==f.accel[i],"gyro and accel survive");
  for(int i=0; i<4; ++i)
    require(g.quat[i]==f.quat[i],"quat survives");

  auto magic = b;
  magic[3] = '2';
  auto hand = b;
  hand[4] = 2;
  require(HandMotion::decode(magic.data(),magic.size(),g)==HandMotion::Status::BadMagic,"bad magic");
  require(HandMotion::decode(b.data(),63,g)==HandMotion::Status::BadSize,"63 bytes");
  require(HandMotion::decode(hand.data(),hand.size(),g)==HandMotion::Status::BadHand,"hand 2");

  Receiver r(offline());
  const auto now = Receiver::Clock::now();
  auto longer = b;
  longer.push_back(0);
  require(!r.accept(magic.data(),magic.size(),now),"receiver drops bad magic");
  require(!r.accept(b.data(),63,now),"receiver drops 63 bytes");
  require(!r.accept(longer.data(),longer.size(),now),"receiver drops 65 bytes");
  require(!r.accept(hand.data(),hand.size(),now),"receiver drops hand 2");
  require(r.malformed()==4,"four bad datagrams counted, got "+std::to_string(r.malformed()));
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
  settle();
  r.tick();
  for(uint8_t h=0; h<HandMotion::HandCount; ++h) {
    const auto& t = r.hand(HandMotion::Hand(h));
    require(t.size()==20 && t.stats().frames==20 && t.stats().lost==0,"20 frames of hand "+std::to_string(h));
    const uint32_t first = h==HandMotion::Left ? 1 : 101;
    for(size_t i=0; i<t.size(); ++i) {
      require(t[i].frame.hand==h && t[i].frame.seq==first+i,"hand and seq order");
      require(t[i].frame.gyro[0]==float(first+i)*0.5f,"payload");
      }
    require(t.last()!=nullptr && t.last()->frame.seq==first+19,"last frame");
    }
  require(r.malformed()==0,"nothing malformed");
  }

// a gap 10 -> 13 is 2 lost; a repeat and a lower seq are dropped
void seqGaps() {
  Receiver r(offline());
  auto at = Receiver::Clock::now();
  require(feed(r,frame(HandMotion::Left,10),at),"seq 10");
  require(feed(r,frame(HandMotion::Left,13),at+=milliseconds(5)),"seq 13");
  require(r.hand(HandMotion::Left).stats().lost==2,"10 -> 13: 2 lost");
  require(!feed(r,frame(HandMotion::Left,13),at+=milliseconds(5)),"repeat dropped");
  require(!feed(r,frame(HandMotion::Left,12),at+=milliseconds(5)),"lower dropped");
  const auto& st = r.hand(HandMotion::Left).stats();
  require(st.dropped==2 && st.frames==2 && st.lost==2,"dropped 2, frames 2, lost still 2");
  require(r.hand(HandMotion::Left).size()==2 && r.hand(HandMotion::Left)[1].frame.seq==13,"history 10, 13");
  require(feed(r,frame(HandMotion::Left,14),at+=milliseconds(5)) && st.lost==2,"14 follows 13 without a loss");
  require(r.hand(HandMotion::Right).stats().frames==0,"the other hand is untouched");
  }

// tick on an empty socket changes nothing and does not block
void emptyTick() {
  Receiver r(anyPort());
  require(r.isOpen(),"open: "+r.error());
  std::vector<double> ms;
  for(int i=0; i<21; ++i) {
    const auto t0 = steady_clock::now();
    r.tick();
    ms.push_back(duration<double,std::milli>(steady_clock::now()-t0).count());
    }
  std::sort(ms.begin(),ms.end());
  require(ms[ms.size()/2]<1.0,"median tick under 1 ms, got "+std::to_string(ms[ms.size()/2]));
  for(uint8_t h=0; h<HandMotion::HandCount; ++h) {
    const auto& t = r.hand(HandMotion::Hand(h));
    require(t.size()==0 && t.last()==nullptr && t.stats().frames==0 && t.stats().dropped==0,"no frames");
    }
  require(r.malformed()==0 && r.isOpen(),"still open, nothing counted");
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
  }

// the socket asks for >= 256 KB; 400 frames sent between two ticks all come
void receiveBuffer() {
  Receiver r(anyPort());
  require(r.isOpen(),"open: "+r.error());
  require(r.receiveBuffer()>=256*1024,"SO_RCVBUF >= 256 KB, got "+std::to_string(r.receiveBuffer()));
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
  }

// a second receiver on a busy port: no crash, an error, tick works
void portInUse() {
  Receiver first(anyPort());
  require(first.isOpen(),"first open: "+first.error());
  Receiver::Config c;
  c.port = first.port();
  Receiver second(c);
  require(!second.isOpen() && !second.error().empty() && second.port()==0,"second reports the busy port");
  second.tick();
  second.tick();
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
  ::unsetenv("OPENGOTHIC_HAND_MOTION");
  ::unsetenv("OPENGOTHIC_HAND_MOTION_PORT");
  require(!cfg.enabled && cfg.port==port,"OPENGOTHIC_HAND_MOTION=0 turns it off");

  Receiver off(cfg);
  off.tick();
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
