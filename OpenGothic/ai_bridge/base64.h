#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

inline std::vector<uint8_t> fromBase64(std::string_view in) {
  static const auto table = [] {
    std::array<int8_t,256> t{};
    t.fill(-1);
    const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int i=0; i<64; ++i)
      t[uint8_t(abc[i])] = int8_t(i);
    return t;
    }();
  std::vector<uint8_t> out;
  out.reserve(in.size()*3/4);
  uint32_t acc  = 0;
  int      bits = 0;
  for(char c:in) {
    const int8_t v = table[uint8_t(c)];
    if(v<0)
      continue;  // padding, line breaks
    acc   = (acc<<6) | uint32_t(v);
    bits += 6;
    if(bits>=8) {
      bits -= 8;
      out.push_back(uint8_t(acc>>bits));
      }
    }
  return out;
  }
