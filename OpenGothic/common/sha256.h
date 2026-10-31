#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// FIPS 180-4 SHA-256. Call finish once, after the last update.
class Sha256 {
  public:
    void update(const void* data, size_t size);
    std::string finish();
    static std::string hash(std::string_view bytes);

  private:
    void compress(const uint8_t* block);

    uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t  buffer[64] = {};
    size_t   buffered = 0;
    uint64_t length = 0;
  };
