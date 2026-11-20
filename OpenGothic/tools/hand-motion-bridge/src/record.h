#pragma once

// The bridge's record (README.md, "Запись"): raw samples as CSV, one row per sample, rows of one packet share the
// hand and the arrival. A "# device <L|R> <name>" line says which device took the hand from that row on.

#include <cstdio>
#include <string>
#include <vector>

#include "motion.h"

namespace Bridge {

struct RecordEvent {
  bool             isDevice = false;
  HandMotion::Hand hand     = HandMotion::Left;
  std::string      device;  // isDevice
  Packet           packet;  // !isDevice
  };

// false and `err` on a file that cannot be read or a row that does not parse
bool readRecord(const std::string& path, std::vector<RecordEvent>& out, std::string& err);

class RecordWriter final {
  public:
    RecordWriter() = default;
    ~RecordWriter();
    RecordWriter(const RecordWriter&) = delete;
    RecordWriter& operator=(const RecordWriter&) = delete;

    bool open(const std::string& path, std::string& err);
    void device(HandMotion::Hand hand, const std::string& name);
    void packet(const Packet& p);
    void flush();

  private:
    std::FILE* f = nullptr;
  };

}
