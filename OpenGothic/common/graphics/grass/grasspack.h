#pragma once

#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

#include "grassdiag.h"
#include "grassparams.h"

namespace Grass {

constexpr std::array<uint32_t, 4> PackSizes = {1, 4, 16, 64};

inline uint32_t parsePackSize(std::string_view text) {
  uint32_t size = 0;
  const auto end = text.data()+text.size();
  const auto [next, error] = std::from_chars(text.data(), end, size);
  if(error==std::errc{} && next==end && std::find(PackSizes.begin(), PackSizes.end(), size)!=PackSizes.end())
    return size;
  return 0;
  }

inline uint32_t& packSize() {
  static uint32_t size = [] {
    if(const char* env = std::getenv("OPENGOTHIC_GRASS_PACK")) {
      if(const auto parsed = parsePackSize(env))
        return parsed;
      Tempest::Log::e("Invalid OPENGOTHIC_GRASS_PACK. Expected 1, 4, 16 or 64. Using 1.");
      }
    return 1u;
    }();
  return size;
  }

inline bool setBenchPack(std::string_view text) {
  const auto size = parsePackSize(text);
  if(size==0)
    return false;
  packSize() = size;
  return true;
  }

constexpr size_t packIndexCount(uint8_t links, uint32_t pack) {
  return size_t(2*links-1)*3*pack;
  }

constexpr size_t packIndexOffset(uint8_t links, uint32_t pack) {
  size_t at = 0;
  for(uint8_t s=MaxSegments; s>links; --s)
    at += packIndexCount(s, pack);
  return at;
  }

inline std::vector<uint16_t> packIndices(uint32_t pack) {
  static_assert(PackSizes.back()*(2*MaxSegments+1)-1 <= std::numeric_limits<uint16_t>::max());
  std::vector<uint16_t> index;
  index.reserve(packIndexOffset(0, pack));
  for(uint8_t s=MaxSegments; s>=1; --s) {
    for(uint32_t copy=0; copy<pack; ++copy) {
      const uint32_t first = copy*(2*s+1);
      for(uint32_t k=0; k+1<s; ++k) {
        const uint32_t v = first+2*k;
        const uint16_t quad[] = {uint16_t(v), uint16_t(v+1), uint16_t(v+2), uint16_t(v+1), uint16_t(v+3), uint16_t(v+2)};
        index.insert(index.end(), std::begin(quad), std::end(quad));
        }
      const uint32_t v = first+2*(s-1);
      const uint16_t tip[] = {uint16_t(v), uint16_t(v+1), uint16_t(v+2)};
      index.insert(index.end(), std::begin(tip), std::end(tip));
      }
    }
  return index;
  }

// One draw has at most 16 spans (grass.vert). Merge adjacent ranges before rounding each span to a pack.
class PackSpans final {
  public:
    PackSpans(std::vector<uint32_t>& table, uint32_t pack)
      :table(table), pack(pack), stride(pack==1 ? 2 : 3) {
      reset();
      }

    void reset() {
      offset = table.size();
      before = length = bladeEnd = 0;
      at = offset;
      }

    void add(uint32_t first, uint32_t count) {
      if(length==0 || first!=bladeEnd) {
        before = instanceEnd();
        length = 0;
        at = table.size();
        table.resize(at+stride);
        table[at+1] = first-before*pack;
        }
      length += count;
      bladeEnd = first+count;
      table[at] = instanceEnd();
      if(pack!=1)
        table[at+2] = bladeEnd;
      }

    uint32_t finish() {
      constexpr size_t MaxSpans = size_t(CellSquares*CellSquares);
      table.resize(offset+stride*MaxSpans, std::numeric_limits<uint32_t>::max());
      return instanceEnd();
      }

  private:
    uint32_t instanceEnd() const { return before+(pack==1 ? length : (length+pack-1)/pack); }

    std::vector<uint32_t>& table;
    const uint32_t pack;
    const size_t stride;
    size_t offset = 0;
    size_t at = 0;
    uint32_t before = 0;
    uint32_t length = 0;
    uint32_t bladeEnd = 0;
  };

}
