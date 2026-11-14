#pragma once

// The "hand motion" frame, version 1 (docs/hand-motion.md): one UDP datagram is one sample of one hand.
// 64 bytes, little-endian. No engine dependency: the sender bridge includes this header as it is.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace HandMotion {

inline constexpr uint16_t DefaultPort = 47821;
inline constexpr size_t   FrameSize   = 64;
inline constexpr char     Magic[4]    = {'G','H','M','1'};

enum Hand : uint8_t {
  Left      = 0,
  Right     = 1,
  HandCount = 2,
  };

enum Flag : uint8_t {
  GyroBiasRemoved = 1u<<0, // the sender has already subtracted the gyro's bias
  AtRest          = 1u<<1, // the sensor is at rest now
  QuatValid       = 1u<<2, // `quat` holds an orientation
  };

// The layout matches the wire: the static_asserts below pin it, encode() and decode() still go byte by byte.
struct Frame {
  char     magic[4] = {'G','H','M','1'};
  uint8_t  hand     = Left;
  uint8_t  flags    = 0;
  uint16_t reserved = 0;
  uint32_t seq      = 0;          // the sender's frame number of this hand, +1 for every frame sent
  uint32_t buttons  = 0;          // bit N: SDL_GamepadButton N
  uint64_t tUs      = 0;          // the sample's time on the sender's monotonic clock, µs; any origin
  float    gyro[3]  = {};         // rad/s, the sensor axes of an SDL gamepad
  float    accel[3] = {};         // m/s²
  float    quat[4]  = {1,0,0,0};  // w, x, y, z; Z points up, against gravity
  };

static_assert(sizeof(float)==4);
static_assert(sizeof(Frame)==FrameSize);
static_assert(offsetof(Frame,magic)   ==0);
static_assert(offsetof(Frame,hand)    ==4);
static_assert(offsetof(Frame,flags)   ==5);
static_assert(offsetof(Frame,reserved)==6);
static_assert(offsetof(Frame,seq)     ==8);
static_assert(offsetof(Frame,buttons) ==12);
static_assert(offsetof(Frame,tUs)     ==16);
static_assert(offsetof(Frame,gyro)    ==24);
static_assert(offsetof(Frame,accel)   ==36);
static_assert(offsetof(Frame,quat)    ==48);

enum class Status : uint8_t {
  Ok,
  BadSize,  // the datagram is not 64 bytes
  BadMagic, // it does not start with GHM1
  BadHand,  // hand is neither 0 nor 1
  };

namespace detail {
  inline void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v>>8);
    }
  inline void put32(uint8_t* p, uint32_t v) {
    for(int i=0; i<4; ++i)
      p[i] = uint8_t(v>>(8*i));
    }
  inline void put64(uint8_t* p, uint64_t v) {
    for(int i=0; i<8; ++i)
      p[i] = uint8_t(v>>(8*i));
    }
  inline void putF(uint8_t* p, float f) {
    uint32_t v = 0;
    std::memcpy(&v,&f,sizeof(v));
    put32(p,v);
    }
  inline uint16_t get16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1]<<8));
    }
  inline uint32_t get32(const uint8_t* p) {
    uint32_t v = 0;
    for(int i=0; i<4; ++i)
      v |= uint32_t(p[i])<<(8*i);
    return v;
    }
  inline uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for(int i=0; i<8; ++i)
      v |= uint64_t(p[i])<<(8*i);
    return v;
    }
  inline float getF(const uint8_t* p) {
    const uint32_t v = get32(p);
    float f = 0;
    std::memcpy(&f,&v,sizeof(f));
    return f;
    }
  }

// Writes FrameSize bytes into `out`.
inline void encode(const Frame& f, void* out) {
  auto* p = static_cast<uint8_t*>(out);
  std::memcpy(p,f.magic,4);
  p[4] = f.hand;
  p[5] = f.flags;
  detail::put16(p+6, f.reserved);
  detail::put32(p+8, f.seq);
  detail::put32(p+12,f.buttons);
  detail::put64(p+16,f.tUs);
  for(int i=0; i<3; ++i)
    detail::putF(p+24+4*i,f.gyro[i]);
  for(int i=0; i<3; ++i)
    detail::putF(p+36+4*i,f.accel[i]);
  for(int i=0; i<4; ++i)
    detail::putF(p+48+4*i,f.quat[i]);
  }

// Reads one datagram; `out` holds the frame only when the answer is Status::Ok.
inline Status decode(const void* data, size_t size, Frame& out) {
  if(size!=FrameSize)
    return Status::BadSize;
  const auto* p = static_cast<const uint8_t*>(data);
  if(std::memcmp(p,Magic,4)!=0)
    return Status::BadMagic;
  if(p[4]>=HandCount)
    return Status::BadHand;
  Frame f;
  std::memcpy(f.magic,p,4);
  f.hand     = p[4];
  f.flags    = p[5];
  f.reserved = detail::get16(p+6);
  f.seq      = detail::get32(p+8);
  f.buttons  = detail::get32(p+12);
  f.tUs      = detail::get64(p+16);
  for(int i=0; i<3; ++i)
    f.gyro[i]  = detail::getF(p+24+4*i);
  for(int i=0; i<3; ++i)
    f.accel[i] = detail::getF(p+36+4*i);
  for(int i=0; i<4; ++i)
    f.quat[i]  = detail::getF(p+48+4*i);
  out = f;
  return Status::Ok;
  }

}
