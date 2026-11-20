#pragma once

// Where the frames go: a UDP socket (POSIX) or the console.

#include <cstdint>
#include <string>

#include "motion.h"

namespace Bridge {

class UdpSender final : public FrameSink {
  public:
    UdpSender() = default;
    ~UdpSender() override;
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // `host`: an IPv4 address or a name
    bool     open(const std::string& host, uint16_t port, std::string& err);
    void     send(const HandMotion::Frame& f) override;
    uint64_t errors() const { return failed; }
    // why the last send failed
    const std::string& lastError() const { return lastErr; }

  private:
    int         sock = -1;
    uint8_t     addr[16] = {}; // sockaddr_in
    uint64_t    failed = 0;
    std::string lastErr;
  };

// a line per frame on stdout
class PrintSink final : public FrameSink {
  public:
    void send(const HandMotion::Frame& f) override;
  };

}
