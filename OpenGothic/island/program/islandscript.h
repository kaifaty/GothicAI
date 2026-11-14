#pragma once

#include "islandcontrol.h"

#include <filesystem>
#include <string>
#include <vector>

namespace island {

// A text track in metres and milliseconds; # starts a comment. See island/scripts/route.txt.
class ScriptTrack final : public ControlSource {
  public:
    explicit ScriptTrack(const std::filesystem::path& file);
    Control next(uint64_t time, uint64_t dt) override;
    bool finished(uint64_t time) const override { return time>=length; }
    const Tempest::Vec3& start() const { return feet; }
    float yaw() const { return angle; }
    struct Shot { uint64_t time; std::string name; };
    const std::vector<Shot>& shots() const { return pictures; }

  private:
    struct Move { uint64_t time; float forward, strafe, turn, pitch; bool run; };
    Tempest::Vec3 feet;
    float angle = 0;
    uint64_t length = 0;
    std::vector<Move> moves;
    std::vector<uint64_t> jumps;
    std::vector<Shot> pictures;
  };

}
