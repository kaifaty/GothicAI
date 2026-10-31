#include "sha256.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace {
// FIPS 180-4, sections 4.2.2 and 6.2.
constexpr uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
  };
}

void Sha256::compress(const uint8_t* block) {
  uint32_t w[64];
  for(size_t i=0;i<16;++i)
    w[i]=(uint32_t(block[4*i])<<24) | (uint32_t(block[4*i+1])<<16) |
         (uint32_t(block[4*i+2])<<8) | uint32_t(block[4*i+3]);
  for(size_t i=16;i<64;++i) {
    const auto x=w[i-15], y=w[i-2];
    const auto s0=std::rotr(x,7)^std::rotr(x,18)^(x>>3);
    const auto s1=std::rotr(y,17)^std::rotr(y,19)^(y>>10);
    w[i]=w[i-16]+s0+w[i-7]+s1;
    }
  auto a=state[0], b=state[1], c=state[2], d=state[3];
  auto e=state[4], f=state[5], g=state[6], h=state[7];
  for(size_t i=0;i<64;++i) {
    const auto s1=std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25);
    const auto ch=(e&f)^(~e&g);
    const auto t1=h+s1+ch+K[i]+w[i];
    const auto s0=std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22);
    const auto maj=(a&b)^(a&c)^(b&c);
    const auto t2=s0+maj;
    h=g; g=f; f=e; e=d+t1;
    d=c; c=b; b=a; a=t1+t2;
    }
  state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
  state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
  }

void Sha256::update(const void* data, size_t size) {
  if(size==0) return;
  if(size>UINT64_MAX/8-length)
    throw std::length_error("SHA-256 input too large");
  length+=size;
  auto bytes=static_cast<const uint8_t*>(data);
  if(buffered!=0) {
    const auto n=std::min(size,sizeof(buffer)-buffered);
    std::memcpy(buffer+buffered,bytes,n);
    buffered+=n; bytes+=n; size-=n;
    if(buffered==sizeof(buffer)) {
      compress(buffer);
      buffered=0;
      }
    }
  while(size>=sizeof(buffer)) {
    compress(bytes);
    bytes+=sizeof(buffer);
    size-=sizeof(buffer);
    }
  if(size!=0) {
    std::memcpy(buffer,bytes,size);
    buffered=size;
    }
  }

std::string Sha256::finish() {
  buffer[buffered++]=0x80;
  if(buffered>56) {
    std::fill(buffer+buffered,buffer+64,0);
    compress(buffer);
    buffered=0;
    }
  std::fill(buffer+buffered,buffer+56,0);
  const auto bits=length*8;
  for(unsigned i=0;i<8;++i)
    buffer[63-i]=uint8_t(bits>>(8*i));
  compress(buffer);
  std::string hex;
  hex.reserve(64);
  for(auto word:state)
    for(int shift=28;shift>=0;shift-=4)
      hex.push_back("0123456789abcdef"[(word>>shift)&15]);
  return hex;
  }

std::string Sha256::hash(std::string_view bytes) {
  Sha256 digest;
  digest.update(bytes.data(),bytes.size());
  return digest.finish();
  }
