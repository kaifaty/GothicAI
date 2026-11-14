#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "input/handmotionframe.h"

// The receiver of hand motion frames (docs/hand-motion.md): a non-blocking UDP socket on 127.0.0.1, read to the
// bottom once a frame by tick(), without a thread of its own. Per hand it keeps the samples of the last second in
// seq order, the last frame, the time it came, the statistics, and puts the sender's clock onto the game's.
class HandMotionReceiver final {
  public:
    using Clock = std::chrono::steady_clock;

    struct Config {
      bool     enabled = true;
      uint16_t port    = HandMotion::DefaultPort; // 0: any free port, port() tells which
      bool     log     = false;                   // a line per hand once a second
      // OPENGOTHIC_HAND_MOTION=0 turns it off, OPENGOTHIC_HAND_MOTION_PORT moves it,
      // OPENGOTHIC_HAND_MOTION_LOG=1 turns the log on; the game adds --no-hand-motion on top (MainWindow)
      static Config fromEnvironment();
      };

    struct Sample {
      HandMotion::Frame frame;
      Clock::time_point received; // when the datagram was read
      Clock::time_point time;     // frame.tUs on the game's clock
      };

    struct Stats {
      uint64_t frames   = 0; // accepted
      uint64_t lost     = 0; // missing by gaps of seq
      uint64_t dropped  = 0; // seq not above the last one: repeated or late
      uint64_t overflow = 0; // pushed out of a full history before they aged
      float    fps      = 0; // accepted per second, over the last second
      };

    static constexpr size_t HistoryCapacity = 512;     // ~1 s at 500 frames per second
    static constexpr auto   HistoryAge      = std::chrono::seconds(1);
    static constexpr auto   ClockWindow     = std::chrono::seconds(2);
    static constexpr int    ReceiveBuffer   = 256*1024; // a pause of 0.5 s at 400 frames per second

    class Track {
      public:
        size_t            size() const { return count; }
        // 0 is the oldest; in seq order
        const Sample&     operator[](size_t i) const { return ring[(begin+i)%ring.size()]; }
        // the last accepted frame, also after it aged out of the history; nullptr before the first
        const Sample*     last() const { return any ? &lastSample : nullptr; }
        Clock::time_point lastReceived() const { return lastSample.received; }
        const Stats&      stats() const { return st; }
        // the sender's time on the game's clock; false before the first frame of the stream
        bool              toGameTime(uint64_t tUs, Clock::time_point& out) const;

      private:
        struct Offset {
          int64_t receivedUs = 0;
          int64_t offsetUs   = 0; // receipt - tUs
          };

        std::vector<Sample> ring = std::vector<Sample>(HistoryCapacity);
        size_t              begin = 0;
        size_t              count = 0;
        Sample              lastSample;
        bool                any       = false;
        uint32_t            lastSeq   = 0;
        // the sliding minimum of the offset over ClockWindow: a frame late on its way does not move the scale
        std::deque<Offset>  offsets;
        Stats               st;
        uint64_t            rateFrames = 0;
        Clock::time_point   rateMark;

      friend class HandMotionReceiver;
      };

    explicit HandMotionReceiver(const Config& cfg);
    ~HandMotionReceiver();

    HandMotionReceiver(const HandMotionReceiver&) = delete;
    HandMotionReceiver& operator=(const HandMotionReceiver&) = delete;

    bool               isOpen() const { return sock>=0; }
    uint16_t           port() const { return boundPort; }
    // SO_RCVBUF as the system reports it after the request
    int                receiveBuffer() const { return rcvBuf; }
    // why the socket is closed; empty when it is open or turned off
    const std::string& error() const { return err; }

    // reads the socket to the bottom, ages the histories, writes the log once a second
    void               tick();
    void               tick(Clock::time_point now);
    // one datagram, read at `received`: false when it is dropped
    bool               accept(const void* data, size_t size, Clock::time_point received);

    const Track&       hand(HandMotion::Hand h) const { return hands[h<HandMotion::HandCount ? h : 0]; }
    // bad size, magic or hand: there is no hand to count them on
    uint64_t           malformed() const { return bad; }

  private:
    void               open();
    void               close();
    void               fail(std::string what);
    void               drain();
    // drops samples older than HistoryAge: not an overflow
    static void        expire(Track& t, Clock::time_point now);
    void               report(Clock::time_point now);

    Config             cfg;
    intptr_t           sock      = -1;
    uint16_t           boundPort = 0;
    int                rcvBuf    = 0;
    [[maybe_unused]] bool wsa    = false; // Windows: WSAStartup succeeded, WSACleanup is due
    std::string        err;
    Track              hands[HandMotion::HandCount];
    uint64_t           bad = 0;
    Clock::time_point  logMark;
  };
