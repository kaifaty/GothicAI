#include "textureflow.h"

#include <algorithm>
#include <cmath>

const char* TextureFlow::name(State state) {
  switch(state) {
    case Waiting:    return "wait";
    case Reading:    return "read";
    case Uploading:  return "upload";
    case Publishing: return "publish";
    case Stale:      return "stale";
    default:         return "none";
    }
  }

TextureFlow::State TextureFlow::state(bool loading, bool stale, Phase phase) {
  if(!loading)
    return Waiting;
  if(stale)
    return Stale;
  switch(phase) {
    case Phase::Queued:
    case Phase::Reading:   return Reading;
    case Phase::Uploading:
    case Phase::Done:      return Uploading;
    case Phase::Ready:     return Publishing;
    }
  return Reading;
  }

size_t TextureFlow::Histogram::bucket(double ms) {
  if(!(ms>=1.0))
    return 0;
  const double b = std::floor(8.0*std::log2(ms))+1.0;
  return size_t(std::min(b,double(Buckets-1)));
  }

double TextureFlow::Histogram::upper(size_t bucket) {
  // bucket b >= 1 holds [2^((b-1)/8), 2^(b/8)) ms
  return std::exp2(double(bucket)/8.0);
  }

void TextureFlow::Histogram::add(double ms) {
  if(!std::isfinite(ms) || ms<0)
    ms = 0;
  ++counts[bucket(ms)];
  ++total;
  maxMs = std::max(maxMs,ms);
  }

double TextureFlow::Histogram::quantile(double q, const Histogram& before) const {
  const uint64_t n = count(before);
  if(n==0)
    return -1;
  // the smallest bucket with at least ceil(q*n) of the times at or below it
  const uint64_t rank = std::max<uint64_t>(1,uint64_t(std::ceil(std::clamp(q,0.0,1.0)*double(n))));
  uint64_t seen = 0;
  for(size_t i=0;i<Buckets;++i) {
    seen += counts[i]-before.counts[i];
    if(seen>=rank)
      return i+1==Buckets ? max(before) : std::min(upper(i),maxMs);
    }
  return maxMs;
  }

double TextureFlow::Histogram::max(const Histogram& before) const {
  if(count(before)==0)
    return -1;
  if(maxMs>before.maxMs)
    return maxMs;
  for(size_t i=Buckets;i-->0;)
    if(counts[i]!=before.counts[i])
      return std::min(upper(i),maxMs);
  return maxMs;
  }

double TextureFlow::Histogram::quantile(double q) const {
  return quantile(q,Histogram());
  }

double TextureFlow::Histogram::max() const {
  return max(Histogram());
  }

double TextureFlow::Bursts::quantile(double q) const {
  return quantile(q,Bursts());
  }

void TextureFlow::Bursts::close() {
  if(size==0)
    return;
  ++counts[std::min<size_t>(size,Buckets-1)];
  ++total;
  maxSize = std::max(maxSize,size);
  size = 0;
  }

void TextureFlow::Bursts::tick(Clock::time_point now) {
  if(size!=0 && now-since>=Window)
    close();
  }

void TextureFlow::Bursts::add(Clock::time_point now) {
  tick(now);
  if(size==0)
    since = now;
  ++size;
  }

double TextureFlow::Bursts::quantile(double q, const Bursts& before) const {
  const uint64_t n = count(before);
  if(n==0)
    return -1;
  const uint64_t rank = std::max<uint64_t>(1,uint64_t(std::ceil(std::clamp(q,0.0,1.0)*double(n))));
  uint64_t seen = 0;
  for(size_t i=0;i<Buckets;++i) {
    seen += counts[i]-before.counts[i];
    if(seen>=rank)
      return i+1==Buckets ? double(max(before)) : double(i);
    }
  return double(maxSize);
  }

uint32_t TextureFlow::Bursts::max(const Bursts& before) const {
  if(count(before)==0)
    return 0;
  if(maxSize>before.maxSize)
    return maxSize;
  for(size_t i=Buckets;i-->0;)
    if(counts[i]!=before.counts[i])
      return i+1==Buckets ? maxSize : uint32_t(i);
  return maxSize;
  }

void TextureFlow::Tracker::integrate(double elapsed) {
  for(size_t i=0;i<States;++i)
    seconds[i] += elapsed*double(count[i]);
  visibleSeconds += elapsed*double(visible);
  aheadSeconds   += elapsed*double(ahead);
  }

uint64_t TextureFlow::Tracker::shortfall() const {
  uint64_t n = 0;
  for(const auto c:count)
    n += c;
  return n;
  }

bool TextureFlow::admits(size_t inFlight, uint64_t bytesInFlight, uint64_t bytes, const Limits& limits) {
  if(inFlight==0)
    return true;
  return inFlight<limits.loads && bytesInFlight+bytes<=limits.bytes;
  }

bool TextureFlow::publishes(size_t published, double elapsedMs, const Limits& limits) {
  return published==0 || elapsedMs<limits.publishMs;
  }
