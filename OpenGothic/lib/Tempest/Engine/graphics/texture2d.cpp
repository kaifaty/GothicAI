#include "texture2d.h"

#include <Tempest/Device>
#include <Tempest/Pixmap>
#include <algorithm>
#include <atomic>

using namespace Tempest;

namespace {
std::atomic<uint64_t> sampledBytes{0};
std::atomic<uint64_t> sampledCount{0};
std::atomic<uint64_t> sampledPeak{0};
}

struct Texture2d::PayloadAccounting {
  explicit PayloadAccounting(uint64_t bytes):bytes(bytes) {
    sampledCount.fetch_add(1,std::memory_order_relaxed);
    const auto live = sampledBytes.fetch_add(bytes,std::memory_order_relaxed)+bytes;
    auto peak = sampledPeak.load(std::memory_order_relaxed);
    while(peak<live && !sampledPeak.compare_exchange_weak(peak,live,std::memory_order_relaxed)) {}
    }

  ~PayloadAccounting() {
    if(!retired) {
      sampledBytes.fetch_sub(bytes,std::memory_order_relaxed);
      sampledCount.fetch_sub(1,std::memory_order_relaxed);
      }
    }

  uint64_t bytes = 0;
  bool retired = false;
  };

Texture2d::Texture2d(Device&, AbstractGraphicsApi::PTexture&& impl, uint32_t w, uint32_t h, uint32_t d, TextureFormat frm,
                     bool trackPayload)
    :impl(std::move(impl)),frm(frm),texW(int(w)),texH(int(h)),texD(int(d)) {
  if(!trackPayload || isEmpty())
    return;
  uint64_t bytes = 0;
  const auto block = Pixmap::blockSizeForFormat(frm);
  for(uint32_t i=0; i<std::max(1u,mipCount()); ++i) {
    const auto size = Pixmap::blockCount(frm,std::max(1u,w>>i),std::max(1u,h>>i));
    bytes += uint64_t(size.w)*uint64_t(size.h)*std::max(1u,d>>i)*block;
    }
  if(bytes!=0)
    accounting=std::make_shared<PayloadAccounting>(bytes);
  }

Texture2d::Texture2d(Texture2d&& other)
    :impl(std::move(other.impl)), frm(other.frm), texW(other.texW), texH(other.texH), texD(other.texD),
     accounting(std::move(other.accounting)) {
  other.texW = 0;
  other.texH = 0;
  other.texD = 0;
  }

Texture2d::~Texture2d() = default;

Texture2d& Texture2d::operator=(Texture2d&& other) {
  std::swap(impl, other.impl);
  std::swap(texW, other.texW);
  std::swap(texH, other.texH);
  std::swap(texD, other.texD);
  std::swap(frm,  other.frm);
  std::swap(accounting, other.accounting);
  return *this;
  }

uint32_t Texture2d::mipCount() const {
  return impl.handler ? impl.handler->mipCount() : 0;
  }

Texture2d Texture2d::shared() const {
  Texture2d texture;
  texture.impl=impl;
  texture.frm=frm;
  texture.texW=texW;
  texture.texH=texH;
  texture.texD=texD;
  texture.accounting=accounting;
  return texture;
  }

void Texture2d::markRetired() {
  if(accounting && !accounting->retired) {
    accounting->retired=true;
    sampledBytes.fetch_sub(accounting->bytes,std::memory_order_relaxed);
    sampledCount.fetch_sub(1,std::memory_order_relaxed);
    }
  }

uint64_t Texture2d::liveBytes() {
  return sampledBytes.load(std::memory_order_relaxed);
  }

uint64_t Texture2d::liveCount() {
  return sampledCount.load(std::memory_order_relaxed);
  }

uint64_t Texture2d::peakBytes() {
  return sampledPeak.load(std::memory_order_relaxed);
  }
