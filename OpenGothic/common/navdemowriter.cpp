#include "navdemowriter.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
void byte(std::vector<uint8_t>& out, uint8_t value) {
  out.push_back(value);
  }

void u32(std::vector<uint8_t>& out, uint32_t value) {
  for(unsigned i=0; i<4; ++i)
    byte(out, uint8_t(value>>(8*i)));
  }

void u64(std::vector<uint8_t>& out, uint64_t value) {
  for(unsigned i=0; i<8; ++i)
    byte(out, uint8_t(value>>(8*i)));
  }

void f32(std::vector<uint8_t>& out, float value) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
  if(!std::isfinite(value))
    throw std::runtime_error("nav demo: non-finite scalar");
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u32(out,bits);
  }

void vec(std::vector<uint8_t>& out, const std::array<float,3>& value) {
  for(float component:value)
    f32(out,component);
  }

void keys(std::vector<uint8_t>& out, const NpcKeys::KeyState& value) {
  if(std::abs(value.forward)>1 || std::abs(value.strafe)>1 || std::abs(value.turn)>1)
    throw std::runtime_error("nav demo: key axis outside [-1,1]");
  f32(out,value.forward);
  f32(out,value.strafe);
  f32(out,value.turn);
  f32(out,value.headYaw);
  f32(out,value.headPitch);
  byte(out,value.walk);
  byte(out,value.jump);
  byte(out,value.climb);
  }

uint32_t age(uint64_t now, uint64_t sampled) {
  if(sampled>now || now-sampled>std::numeric_limits<uint32_t>::max())
    throw std::runtime_error("nav demo: invalid eye timestamp");
  return uint32_t(now-sampled);
  }

void layer(std::vector<uint8_t>& out, const NavigationMemory::Layer& value,
           uint32_t expected, float cellCm, uint32_t formatVersion) {
  if(value.size!=expected || value.cells.size()!=size_t(expected)*expected || value.cellCm!=cellCm)
    throw std::runtime_error("nav demo: incorrect memory shape or resolution");
  for(const auto& cell:value.cells) {
    const float channels[] = {cell.confidence,cell.free,cell.wall,cell.ledge,
                              cell.drop,cell.water,cell.movable};
    for(float c:channels) {
      if(!std::isfinite(c) || c<0 || c>1)
        throw std::runtime_error("nav demo: memory channel outside [0,1]");
      byte(out,uint8_t(std::lround(c*255.f)));
      }
    if(!std::isfinite(cell.familiar) || cell.familiar<0 || cell.familiar>1)
      throw std::runtime_error("nav demo: familiar channel outside [0,1]");
    if(formatVersion==3)
      byte(out,uint8_t(std::lround(cell.familiar*255.f)));
    else if(cell.familiar!=0)
      throw std::runtime_error("nav demo: familiar memory requires format version 3");
    const bool known = std::isfinite(cell.groundHeightCm);
    if(!known && !std::isnan(cell.groundHeightCm))
      throw std::runtime_error("nav demo: infinite memory ground height");
    f32(out,known ? cell.groundHeightCm : 0.f);
    byte(out,known);
    }
  }
}

NavDemoWriter::NavDemoWriter(const std::string& path, uint32_t formatVersion):fileVersion(formatVersion) {
  if(fileVersion!=1 && fileVersion!=2 && fileVersion!=3)
    throw std::invalid_argument("nav demo: unsupported format version");
  buffer.reserve(recordBytesForVersion(fileVersion));
  stream = gzopen(path.c_str(),"wb1");
  if(stream==nullptr)
    throw std::runtime_error("nav demo: cannot create "+path);
  // A large compression buffer avoids many small filesystem writes.
  if(gzbuffer(static_cast<gzFile>(stream),256*1024)!=0) {
    gzclose(static_cast<gzFile>(stream));
    stream = nullptr;
    throw std::runtime_error("nav demo: cannot allocate compression buffer");
    }
  const char magic[] = "NAVDEMO";
  buffer.insert(buffer.end(),magic,magic+8);
  u32(buffer,fileVersion);
  u32(buffer,recordBytesForVersion(fileVersion));
  u32(buffer,rayCount);
  u32(buffer,nearSize);
  u32(buffer,farSize);
  u32(buffer,100);
  try {
    write(buffer);
    }
  catch(...) {
    gzclose(static_cast<gzFile>(stream));
    stream = nullptr;
    throw;
    }
  }

NavDemoWriter::~NavDemoWriter() {
  if(stream!=nullptr)
    gzclose(static_cast<gzFile>(stream));
  }

void NavDemoWriter::write(const std::vector<uint8_t>& data) {
  if(stream==nullptr)
    throw std::runtime_error("nav demo: write after finish");
  const int written = gzwrite(static_cast<gzFile>(stream),data.data(),unsigned(data.size()));
  if(written!=int(data.size())) {
    int code = Z_OK;
    const char* message = gzerror(static_cast<gzFile>(stream),&code);
    throw std::runtime_error(std::string("nav demo: write failed: ")+message);
    }
  }

double NavDemoWriter::append(const Snapshot& sample) {
  const auto started = std::chrono::steady_clock::now();
  if(sample.eye==nullptr || sample.eye->size()!=rayCount || sample.memory==nullptr)
    throw std::runtime_error("nav demo: missing eye or memory observation");
  if(count>0 && (sample.gameMs<=previousMs || sample.gameMs-previousMs!=100))
    throw std::runtime_error("nav demo: decisions must be exactly 100 ms apart");
  if(sample.memory->pose.gameMs!=sample.gameMs)
    throw std::runtime_error("nav demo: stale memory pose");
  buffer.clear();
  u64(buffer,sample.gameMs);
  vec(buffer,sample.position);
  f32(buffer,sample.bodyYawDeg);
  vec(buffer,sample.body.velocityCmS);
  for(uint8_t flag:sample.body.flags) {
    if(flag>1)
      throw std::runtime_error("nav demo: invalid body flag");
    byte(buffer,flag);
    }
  vec(buffer,sample.body.groundNormal);
  f32(buffer,sample.body.groundSlopeDeg);
  f32(buffer,sample.body.headYawDeg);
  f32(buffer,sample.body.headPitchDeg);
  vec(buffer,fileVersion>=2 ? sample.roads.carrotDirection : sample.roads.nextDirection);
  f32(buffer,fileVersion>=2 ? sample.roads.carrotDistanceCm : sample.roads.nextDistanceCm);
  vec(buffer,sample.roads.goalDirection);
  f32(buffer,sample.roads.goalDistanceCm);
  f32(buffer,fileVersion>=2 ? sample.roads.remainingKnownCm : sample.roads.remainingVisualCm);
  byte(buffer,fileVersion>=2 ? sample.roads.beyondKnown : sample.roads.goalInKnown);
  keys(buffer,sample.previous);
  keys(buffer,sample.keys);
  for(const auto& ray:*sample.eye) {
    if(ray.distanceCm<0 || ray.normalY<-1 || ray.normalY>1 || uint8_t(ray.kind)>5)
      throw std::runtime_error("nav demo: eye range or kind invalid");
    f32(buffer,ray.distanceCm);
    byte(buffer,uint8_t(ray.kind));
    f32(buffer,ray.normalY);
    u32(buffer,age(sample.gameMs,ray.sampledAtMs));
    u32(buffer,age(sample.gameMs,ray.nearSampledAtMs));
    byte(buffer,ray.complete);
    byte(buffer,ray.hit);
    }
  layer(buffer,sample.memory->near,nearSize,NavigationMemory::Constants::nearCellCm,fileVersion);
  layer(buffer,sample.memory->far,farSize,NavigationMemory::Constants::farCellCm,fileVersion);
  vec(buffer,sample.teacherCorner);
  f32(buffer,sample.teacherPathCm);
  if(buffer.size()!=recordBytesForVersion(fileVersion))
    throw std::runtime_error("nav demo: internal record layout mismatch");
  write(buffer);
  ++count;
  previousMs = sample.gameMs;
  return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count();
  }

void NavDemoWriter::finish() {
  if(stream==nullptr)
    return;
  const int status = gzclose(static_cast<gzFile>(stream));
  stream = nullptr;
  if(status!=Z_OK)
    throw std::runtime_error("nav demo: failed to flush or close compressed file");
  }
