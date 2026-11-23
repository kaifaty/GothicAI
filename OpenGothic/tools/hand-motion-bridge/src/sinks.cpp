#include "sinks.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace Bridge {

static_assert(sizeof(sockaddr_in)<=16);

UdpSender::~UdpSender() {
  if(sock>=0)
    ::close(sock);
  }

bool UdpSender::open(const std::string& host, uint16_t port, std::string& err) {
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port   = htons(port);
  if(inet_pton(AF_INET,host.c_str(),&a.sin_addr)!=1) {
    addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if(const int e = getaddrinfo(host.c_str(),nullptr,&hints,&res); e!=0 || res==nullptr) {
      err = "cannot resolve "+host+": "+gai_strerror(e);
      return false;
      }
    a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    }
  sock = ::socket(AF_INET,SOCK_DGRAM,0);
  if(sock<0) {
    err = std::string("socket: ")+std::strerror(errno);
    return false;
    }
  std::memcpy(addr,&a,sizeof(a));
  return true;
  }

void UdpSender::send(const HandMotion::Frame& f) {
  uint8_t buf[HandMotion::FrameSize];
  HandMotion::encode(f,buf);
  sockaddr_in a;
  std::memcpy(&a,addr,sizeof(a));
  if(::sendto(sock,buf,sizeof(buf),0,reinterpret_cast<const sockaddr*>(&a),sizeof(a))!=ssize_t(sizeof(buf))) {
    ++failed;
    lastErr = std::strerror(errno);
    }
  }

void PrintSink::send(const HandMotion::Frame& f) {
  Quat q{f.quat[0],f.quat[1],f.quat[2],f.quat[3]};
  double roll = 0, pitch = 0, yaw = 0;
  toEuler(q,roll,pitch,yaw);
  std::printf("%c seq %u t %llu flags %u buttons %u gyro %.4f %.4f %.4f accel %.3f %.3f %.3f "
              "quat %.5f %.5f %.5f %.5f roll %.2f pitch %.2f yaw %.2f\n",
              f.hand==HandMotion::Left ? 'L' : 'R', unsigned(f.seq), (unsigned long long)f.tUs,
              unsigned(f.flags), unsigned(f.buttons), f.gyro[0], f.gyro[1], f.gyro[2],
              f.accel[0], f.accel[1], f.accel[2], f.quat[0], f.quat[1], f.quat[2], f.quat[3], roll, pitch, yaw);
  }

}
