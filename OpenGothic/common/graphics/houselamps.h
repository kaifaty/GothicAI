#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#include "graphics/houselights.h"
#include "graphics/lightgroup.h"

// The lamps of the world's houses (graphics/houselights.h): a lamp of the world that stands inside a house's box
// goes out before the house's bedtime and comes back in the morning. The hour is the one the scene block gives the
// windows' shader, so both tell the same evening. Lamps that stand together (a candle's two lights) are one
// fixture and go out together; the flame drawn at a fixture goes with it. A fire with its own particles (a hearth)
// and the lamps at it burn through the night: a hearth is banked, not put out.
// OPENGOTHIC_HOUSES=0: no houses, the lamps burn round the clock
class HouseLamps final {
  public:
    // lamps this close are one fixture; a flame this close to a lamp is the fixture's, cm
    static constexpr float FixtureReach = 80.f;

    HouseLamps() = default;
    // the files of houses.d in the world's content folder (empty: no content), sorted; none: no houses
    explicit HouseLamps(const std::filesystem::path& worldContentDir);

    const std::vector<HouseLights::House>& houses() const { return list; }

    // A lamp of the world that burns; one outside every house is left alone. fire: it carries a fire's particles.
    // The lamp must outlive its adoption
    void adopt(LightGroup::Light& light, const Tempest::Vec3& pos, uint32_t id, bool fire);
    void drop (LightGroup::Light& light);
    // The picture of a flame (a candle's decal): show(false) when the lamps at it go out. owner names it for drop
    void adoptFlame(const void* owner, const Tempest::Vec3& pos, std::function<void(bool)> show);
    void dropFlame (const void* owner);
    void tick (float dayHour);

  private:
    struct Lamp final {
      LightGroup::Light* light = nullptr;
      Tempest::Vec3      pos;
      uint32_t           house = 0;
      uint32_t           id    = 0;
      float              turn  = 0;
      bool               fire  = false;
      bool               on    = true;
      };
    struct Flame final {
      const void*               owner = nullptr;
      Tempest::Vec3             pos;
      std::function<void(bool)> show;
      size_t                    lamp = size_t(-1);
      bool                      on   = true;
      };

    void rank();

    std::vector<HouseLights::House> list;
    std::vector<Lamp>               lamps;
    std::vector<Flame>              flames;
    bool                            ranked = true;
    std::mutex                      sync; // lamps come and go with their vobs, also off the main thread
  };
