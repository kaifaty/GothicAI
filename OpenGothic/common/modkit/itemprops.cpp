#include "itemprops.h"

#include "savearchive.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace modkit {

namespace {

constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime  = 1099511628211ull;

void hashBytes(uint64_t& h, const void* data, size_t len) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  for(size_t i = 0; i < len; ++i) {
    h ^= uint64_t(p[i]);
    h *= FnvPrime;
    }
  }

template<class T>
void hashPod(uint64_t& h, T v) noexcept {
  hashBytes(h, &v, sizeof(T));
  }

uint32_t canonicalFloatBits(float v) noexcept {
  if(std::isnan(v))
    return 0x7FC00000u;
  if(v == 0.0f)
    return 0u; // normalize -0.0f to +0.0f
  return std::bit_cast<uint32_t>(v);
  }

uint64_t canonicalDoubleBits(double v) noexcept {
  if(std::isnan(v))
    return 0x7FF8000000000000ull;
  if(v == 0.0)
    return 0ull;
  return std::bit_cast<uint64_t>(v);
  }

void hashOptFloat(uint64_t& h, const std::optional<float>& v) noexcept {
  const uint8_t present = v.has_value() ? 1u : 0u;
  hashPod(h, present);
  if(v.has_value())
    hashPod(h, canonicalFloatBits(*v));
  }

bool optFloatEqual(const std::optional<float>& a, const std::optional<float>& b) noexcept {
  if(a.has_value() != b.has_value())
    return false;
  if(!a.has_value())
    return true;
  return canonicalFloatBits(*a) == canonicalFloatBits(*b);
  }

bool propValueEqual(const ItemPropValue& a, const ItemPropValue& b) noexcept {
  if(a.index() != b.index())
    return false;
  switch(a.index()) {
    case 0: return std::get<int64_t>(a) == std::get<int64_t>(b);
    case 1: return canonicalDoubleBits(std::get<double>(a)) == canonicalDoubleBits(std::get<double>(b));
    case 2: return std::get<bool>(a) == std::get<bool>(b);
    case 3: return std::get<std::string>(a) == std::get<std::string>(b);
    default: return false;
    }
  }

enum class PropTag : uint8_t {
  Int64  = 0,
  Double = 1,
  Bool   = 2,
  String = 3,
  };

} // namespace

bool ItemInstanceProps::empty() const noexcept {
  return !quality.has_value() &&
         !durability.has_value() &&
         !maxDurability.has_value() &&
         !temperature.has_value() &&
         !unique &&
         custom.empty();
  }

uint64_t ItemInstanceProps::stackKey(uint32_t archetypeId) const noexcept {
  if(unique)
    return 0;
  uint64_t h = FnvOffset;
  hashPod(h, archetypeId);
  hashOptFloat(h, quality);
  hashOptFloat(h, durability);
  hashOptFloat(h, maxDurability);
  hashOptFloat(h, temperature);
  const uint32_t customSize = uint32_t(custom.size());
  hashPod(h, customSize);
  for(const auto& [k, v] : custom) {
    const uint32_t klen = uint32_t(k.size());
    hashPod(h, klen);
    hashBytes(h, k.data(), k.size());
    const uint8_t tag = uint8_t(v.index());
    hashPod(h, tag);
    switch(v.index()) {
      case 0:
        hashPod(h, std::get<int64_t>(v));
        break;
      case 1:
        hashPod(h, canonicalDoubleBits(std::get<double>(v)));
        break;
      case 2:
        hashPod(h, uint8_t(std::get<bool>(v) ? 1u : 0u));
        break;
      case 3: {
        const auto& s = std::get<std::string>(v);
        const uint32_t slen = uint32_t(s.size());
        hashPod(h, slen);
        hashBytes(h, s.data(), s.size());
        break;
        }
      }
    }
  return h != 0 ? h : 1u;
  }

bool ItemInstanceProps::equalsCanonical(const ItemInstanceProps& other) const noexcept {
  if(unique != other.unique)
    return false;
  if(!optFloatEqual(quality, other.quality) ||
     !optFloatEqual(durability, other.durability) ||
     !optFloatEqual(maxDurability, other.maxDurability) ||
     !optFloatEqual(temperature, other.temperature))
    return false;
  if(custom.size() != other.custom.size())
    return false;
  auto itA = custom.begin();
  auto itB = other.custom.begin();
  for(; itA != custom.end(); ++itA, ++itB) {
    if(itA->first != itB->first || !propValueEqual(itA->second, itB->second))
      return false;
    }
  return true;
  }

bool ItemInstanceProps::canStack(const ItemInstanceProps* a, const ItemInstanceProps* b) noexcept {
  const bool aEmpty = (a == nullptr || a->empty());
  const bool bEmpty = (b == nullptr || b->empty());
  if(aEmpty && bEmpty)
    return true;
  if(aEmpty != bEmpty)
    return false;
  if(a->unique || b->unique)
    return false;
  return a->equalsCanonical(*b);
  }

std::vector<std::pair<std::string, std::string>> ItemInstanceProps::tooltipRows() const {
  std::vector<std::pair<std::string, std::string>> rows;
  char buf[64];
  if(quality.has_value()) {
    const int pct = int(std::lround(*quality * 100.0f));
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    rows.emplace_back("Quality", buf);
    }
  if(durability.has_value()) {
    if(maxDurability.has_value()) {
      std::snprintf(buf, sizeof(buf), "%.0f / %.0f", double(*durability), double(*maxDurability));
      } else {
      std::snprintf(buf, sizeof(buf), "%.0f", double(*durability));
      }
    rows.emplace_back("Durability", buf);
    }
  if(temperature.has_value()) {
    std::snprintf(buf, sizeof(buf), "%.0f C", double(*temperature));
    rows.emplace_back("Temperature", buf);
    }
  return rows;
  }

void ItemInstanceProps::save(SaveArchive& fout) const {
  uint8_t mask = 0;
  if(quality.has_value())       mask |= 1u << 0;
  if(durability.has_value())    mask |= 1u << 1;
  if(maxDurability.has_value()) mask |= 1u << 2;
  if(temperature.has_value())   mask |= 1u << 3;
  if(unique)                    mask |= 1u << 4;

  fout.write(FormatVersion, mask);
  if(quality.has_value())       fout.write(*quality);
  if(durability.has_value())    fout.write(*durability);
  if(maxDurability.has_value()) fout.write(*maxDurability);
  if(temperature.has_value())   fout.write(*temperature);

  fout.write(uint32_t(custom.size()));
  for(const auto& [k, v] : custom) {
    fout.write(k);
    const auto tag = static_cast<uint8_t>(v.index());
    fout.write(tag);
    switch(v.index()) {
      case 0:
        fout.write(std::bit_cast<uint64_t>(std::get<int64_t>(v)));
        break;
      case 1:
        fout.write(std::bit_cast<uint64_t>(std::get<double>(v)));
        break;
      case 2:
        fout.write(std::get<bool>(v));
        break;
      case 3:
        fout.write(std::get<std::string>(v));
        break;
      }
    }
  }

ItemInstanceProps ItemInstanceProps::load(SaveArchive& fin) {
  uint16_t ver  = 0;
  uint8_t  mask = 0;
  fin.read(ver, mask);
  if(ver != FormatVersion)
    throw std::runtime_error("Unsupported ItemInstanceProps format version: " + std::to_string(ver));

  ItemInstanceProps props;
  if(mask & (1u << 0)) { float v = 0; fin.read(v); props.quality       = v; }
  if(mask & (1u << 1)) { float v = 0; fin.read(v); props.durability    = v; }
  if(mask & (1u << 2)) { float v = 0; fin.read(v); props.maxDurability = v; }
  if(mask & (1u << 3)) { float v = 0; fin.read(v); props.temperature   = v; }
  props.unique = (mask & (1u << 4)) != 0;

  uint32_t count = 0;
  fin.read(count);
  for(uint32_t i = 0; i < count; ++i) {
    std::string key;
    uint8_t tag = 0;
    fin.read(key, tag);
    switch(static_cast<PropTag>(tag)) {
      case PropTag::Int64: {
        uint64_t raw = 0;
        fin.read(raw);
        props.custom.emplace(std::move(key), std::bit_cast<int64_t>(raw));
        break;
        }
      case PropTag::Double: {
        uint64_t raw = 0;
        fin.read(raw);
        props.custom.emplace(std::move(key), std::bit_cast<double>(raw));
        break;
        }
      case PropTag::Bool: {
        bool v = false;
        fin.read(v);
        props.custom.emplace(std::move(key), v);
        break;
        }
      case PropTag::String: {
        std::string v;
        fin.read(v);
        props.custom.emplace(std::move(key), std::move(v));
        break;
        }
      default:
        throw std::runtime_error("Unsupported ItemInstanceProps value tag: " + std::to_string(tag));
      }
    }
  return props;
  }

} // namespace modkit
