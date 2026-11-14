#if defined(_WIN32)
// before anything that may pull <windows.h>
#include <winsock2.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "input/handmotion.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std::chrono;

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
using SockLen      = int;

int lastError() { return WSAGetLastError(); }

std::string errorText(int e) {
  return "WSA error " + std::to_string(e);
  }

void closeSocket(intptr_t s) { ::closesocket(SocketHandle(s)); }
#else
using SocketHandle = int;
using SockLen      = socklen_t;

int lastError() { return errno; }

std::string errorText(int e) {
  return std::strerror(e);
  }

void closeSocket(intptr_t s) { ::close(int(s)); }
#endif

// a flood never holds the frame: what is left waits for the next tick
constexpr int MaxPerTick = 4096;

bool isOff(const char* v) {
  return v!=nullptr && (std::strcmp(v,"0")==0 || std::strcmp(v,"false")==0);
  }

bool isOn(const char* v) {
  return v!=nullptr && *v!='\0' && !isOff(v);
  }

int64_t toUs(HandMotionReceiver::Clock::time_point t) {
  return duration_cast<microseconds>(t.time_since_epoch()).count();
  }

// roll about X, pitch about Y, yaw about Z (Z up), degrees
void toEuler(const float q[4], double& roll, double& pitch, double& yaw) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  constexpr double deg = 180.0/3.14159265358979323846;
  roll  = std::atan2(2*(w*x + y*z), 1 - 2*(x*x + y*y))*deg;
  pitch = std::asin(std::clamp(2*(w*y - z*x), -1.0, 1.0))*deg;
  yaw   = std::atan2(2*(w*z + x*y), 1 - 2*(y*y + z*z))*deg;
  }

}

HandMotionReceiver::Config HandMotionReceiver::Config::fromEnvironment() {
  Config c;
  c.enabled = !isOff(std::getenv("OPENGOTHIC_HAND_MOTION"));
  c.log     = isOn(std::getenv("OPENGOTHIC_HAND_MOTION_LOG"));
  if(const char* p = std::getenv("OPENGOTHIC_HAND_MOTION_PORT"); p!=nullptr && *p!='\0') {
    char* end = nullptr;
    const unsigned long v = std::strtoul(p,&end,10);
    if(end!=p && *end=='\0' && v<=65535)
      c.port = uint16_t(v); else
      Tempest::Log::e("[hand-motion] OPENGOTHIC_HAND_MOTION_PORT is not a port: \"",p,"\", using ",c.port);
    }
  return c;
  }

bool HandMotionReceiver::Track::toGameTime(uint64_t tUs, Clock::time_point& out) const {
  if(offsets.empty())
    return false;
  const auto us = int64_t(tUs + uint64_t(offsets.front().offsetUs));
  out = Clock::time_point(duration_cast<Clock::duration>(microseconds(us)));
  return true;
  }

HandMotionReceiver::HandMotionReceiver(const Config& config)
  :cfg(config) {
  const auto now = Clock::now();
  logMark = now;
  for(auto& t:hands)
    t.rateMark = now;
  if(cfg.enabled)
    open();
  }

HandMotionReceiver::~HandMotionReceiver() {
  close();
#if defined(_WIN32)
  if(wsa)
    WSACleanup();
#endif
  }

void HandMotionReceiver::open() {
#if defined(_WIN32)
  WSADATA data = {};
  if(const int e = WSAStartup(MAKEWORD(2,2),&data); e!=0) {
    fail("WSAStartup: "+errorText(e));
    return;
    }
  wsa = true;
#endif
  const SocketHandle s = ::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
#if defined(_WIN32)
  if(s==INVALID_SOCKET) {
#else
  if(s<0) {
#endif
    fail("socket: "+errorText(lastError()));
    return;
    }
  sock = intptr_t(s);

#if defined(_WIN32)
  u_long nonBlocking = 1;
  if(::ioctlsocket(s,FIONBIO,&nonBlocking)!=0) {
    fail("non-blocking: "+errorText(lastError()));
    return;
    }
#else
  const int fl = ::fcntl(s,F_GETFL,0);
  if(fl<0 || ::fcntl(s,F_SETFL,fl|O_NONBLOCK)<0) {
    fail("non-blocking: "+errorText(lastError()));
    return;
    }
  ::fcntl(s,F_SETFD,FD_CLOEXEC);
#endif

  // ask for a buffer that outlives a pause of the game; never shrink a larger default (macOS gives 768 KB)
  auto readBuffer = [&] {
    int     v   = 0;
    SockLen len = SockLen(sizeof(v));
    if(::getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&v),&len)!=0)
      return 0;
    return v;
    };
  rcvBuf = readBuffer();
  if(rcvBuf<ReceiveBuffer) {
    const int want = ReceiveBuffer;
    ::setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&want),SockLen(sizeof(want)));
    rcvBuf = readBuffer();
    if(rcvBuf<ReceiveBuffer)
      Tempest::Log::i("[hand-motion] receive buffer ",rcvBuf," bytes, asked for ",ReceiveBuffer);
    }

  // only the loopback; written byte by byte, in network order
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  auto* port = reinterpret_cast<uint8_t*>(&addr.sin_port);
  port[0] = uint8_t(cfg.port>>8);
  port[1] = uint8_t(cfg.port&0xFF);
  auto* ip = reinterpret_cast<uint8_t*>(&addr.sin_addr);
  ip[0] = 127;
  ip[1] = 0;
  ip[2] = 0;
  ip[3] = 1;
  if(::bind(s,reinterpret_cast<const sockaddr*>(&addr),SockLen(sizeof(addr)))!=0) {
    fail("bind 127.0.0.1:"+std::to_string(cfg.port)+": "+errorText(lastError()));
    return;
    }

  sockaddr_in bound = {};
  SockLen     len   = SockLen(sizeof(bound));
  if(::getsockname(s,reinterpret_cast<sockaddr*>(&bound),&len)!=0) {
    fail("getsockname: "+errorText(lastError()));
    return;
    }
  const auto* bp = reinterpret_cast<const uint8_t*>(&bound.sin_port);
  boundPort = uint16_t((bp[0]<<8) | bp[1]);
  if(cfg.log)
    Tempest::Log::i("[hand-motion] listening on 127.0.0.1:",boundPort);
  }

void HandMotionReceiver::close() {
  if(sock>=0)
    closeSocket(sock);
  sock = -1;
  }

void HandMotionReceiver::fail(std::string what) {
  // one line, once: the game goes on without the receiver and does not retry
  err = std::move(what);
  Tempest::Log::e("[hand-motion] off: ",err);
  close();
  boundPort = 0;
  }

void HandMotionReceiver::tick() {
  if(sock>=0)
    drain();
  tick(Clock::now());
  }

void HandMotionReceiver::expire(Track& t, Clock::time_point now) {
  while(t.count>0 && now-t.ring[t.begin].received>HistoryAge) {
    t.begin = (t.begin+1)%HistoryCapacity;
    --t.count;
    }
  }

void HandMotionReceiver::tick(Clock::time_point now) {
  for(auto& t:hands) {
    expire(t,now);
    const auto dt = now-t.rateMark;
    if(dt>=seconds(1)) {
      t.st.fps      = float(double(t.st.frames-t.rateFrames)/duration<double>(dt).count());
      t.rateFrames  = t.st.frames;
      t.rateMark    = now;
      }
    }
  if(cfg.log && now-logMark>=seconds(1)) {
    logMark = now;
    report(now);
    }
  }

void HandMotionReceiver::drain() {
  uint8_t buf[256];
  for(int i=0; i<MaxPerTick; ++i) {
#if defined(_WIN32)
    const int n = ::recv(SocketHandle(sock),reinterpret_cast<char*>(buf),int(sizeof(buf)),0);
#else
    const ssize_t n = ::recv(int(sock),buf,sizeof(buf),0);
#endif
    const auto at = Clock::now();
    if(n>=0) {
      // longer datagrams come cut to the buffer, an empty one is 0: both fail the size
      accept(buf,size_t(n),at);
      continue;
      }
    const int e = lastError();
#if defined(_WIN32)
    if(e==WSAEWOULDBLOCK)
      return;
    if(e==WSAEMSGSIZE) {
      ++bad;
      continue;
      }
    if(e==WSAECONNRESET || e==WSAEINTR)
      continue; // an ICMP answer to some earlier send: nothing lost here
#else
    if(e==EAGAIN || e==EWOULDBLOCK)
      return;
    if(e==EINTR)
      continue;
#endif
    fail("recv: "+errorText(e));
    return;
    }
  }

bool HandMotionReceiver::accept(const void* data, size_t size, Clock::time_point received) {
  HandMotion::Frame f;
  if(HandMotion::decode(data,size,f)!=HandMotion::Status::Ok) {
    ++bad;
    return false;
    }
  Track& t = hands[f.hand];
  if(t.any) {
    // serial arithmetic: survives the wrap of the 32-bit counter
    const auto step = int32_t(f.seq-t.lastSeq);
    if(step<=0) {
      ++t.st.dropped;
      return false;
      }
    t.st.lost += uint64_t(step-1);
    }
  t.lastSeq = f.seq;
  ++t.st.frames;

  // the sliding minimum of "receipt - tUs" over the last ClockWindow: the least delayed frame sets the scale
  const int64_t recvUs = toUs(received);
  const auto    offset = int64_t(uint64_t(recvUs)-f.tUs);
  while(!t.offsets.empty() && t.offsets.back().offsetUs>=offset)
    t.offsets.pop_back();
  t.offsets.push_back({recvUs,offset});
  const int64_t windowUs = duration_cast<microseconds>(ClockWindow).count();
  while(t.offsets.front().receivedUs<recvUs-windowUs)
    t.offsets.pop_front();

  Sample s;
  s.frame    = f;
  s.received = received;
  t.toGameTime(f.tUs,s.time);

  // what aged while the game did not read (a pause, a load) leaves before the new frames, not as overflow
  expire(t,received);
  const size_t cap = HistoryCapacity;
  if(t.count==cap) {
    t.begin = (t.begin+1)%cap;
    --t.count;
    ++t.st.overflow;
    }
  t.ring[(t.begin+t.count)%cap] = s;
  ++t.count;
  t.lastSample = s;
  t.any        = true;
  return true;
  }

void HandMotionReceiver::report(Clock::time_point now) {
  static const char* name[HandMotion::HandCount] = {"left","right"};
  char line[256];
  for(size_t h=0; h<HandMotion::HandCount; ++h) {
    const Track& t = hands[h];
    if(!t.any) {
      Tempest::Log::i("[hand-motion] ",name[h],": no frames");
      continue;
      }
    const auto  ago = duration_cast<milliseconds>(now-t.lastSample.received).count();
    const auto& f   = t.lastSample.frame;
    char quat[64] = "quat -";
    if(f.flags & HandMotion::QuatValid) {
      double roll = 0, pitch = 0, yaw = 0;
      toEuler(f.quat,roll,pitch,yaw);
      std::snprintf(quat,sizeof(quat),"roll %.1f pitch %.1f yaw %.1f",roll,pitch,yaw);
      }
    std::snprintf(line,sizeof(line),"[hand-motion] %s: %.1f fps, lost %llu, dropped %llu, overflow %llu, last %lld ms ago, %s",
                  name[h],double(t.st.fps),
                  static_cast<unsigned long long>(t.st.lost),
                  static_cast<unsigned long long>(t.st.dropped),
                  static_cast<unsigned long long>(t.st.overflow),
                  static_cast<long long>(ago),quat);
    Tempest::Log::i(line);
    }
  if(bad>0)
    Tempest::Log::i("[hand-motion] bad datagrams: ",bad);
  }
