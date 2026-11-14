#pragma once

// What the island gives the engine in place of Gothic (engine layers, step 8.6): the command line, the settings, the
// camera's presets and the game's services with a clock. The pattern is tests/platform_services.h; the program has
// its own, as nothing of tests/ is a part of it.
//
// The order they need each other in:
//   Arguments  before Resources: Resources reads the command line in its constructor;
//   Options    before the physics and the renderer: both read the settings;
//   Services   after Resources has its device, when there is one: Shaders read the device's features.

#include "camerapresets.h"
#include "commandline.h"
#include "gameservices.h"
#include "settings.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

class DynamicWorld;
class Shaders;

namespace island {

// The command line of the engine: -g names the island's folder, so the installation is never searched for.
// Render options (bindless, ray query, VSM) stay at their defaults for the device.
class Arguments final {
  public:
    explicit Arguments(const std::filesystem::path& root);

  private:
    std::string               path;
    std::array<const char*,3> argv;
    CommandLine               cmd;
  };

// Settings with the engine's defaults: no ini file is read (both would lie in an absent folder) and none is written.
class Options final {
  public:
    explicit Options(const std::filesystem::path& root);

  private:
    Settings settings;
  };

// The island's one camera, a third-person view behind the hero, for every mode: Camera reads a table and the island
// has no camera modes. Gothic's CAMMODNORMAL: 3 m behind, 30 degrees above, collision with the ground.
class CameraTable final : public CameraPresets {
  public:
    CameraTable();

    const Def::Camera& dialogCam()    const override { return cam; }
    const Def::Camera& inventoryCam() const override { return cam; }
    const Def::Camera& stdCam()       const override { return cam; }
    const Def::Camera& fpCam()        const override { return cam; }
    const Def::Camera& backCam()      const override { return cam; }
    const Def::Camera& meleeCam()     const override { return cam; }
    const Def::Camera& rangeCam()     const override { return cam; }
    const Def::Camera& mageCam()      const override { return cam; }
    const Def::Camera& deathCam()     const override { return cam; }
    const Def::Camera& swimCam()      const override { return cam; }
    const Def::Camera& diveCam()      const override { return cam; }
    const Def::Camera& fallCam()      const override { return cam; }
    const Def::Camera& mobsiCam(std::string_view, std::string_view) const override { return cam; }

  private:
    Def::Camera cam{};
  };

// The game's side of the engine: the view, the camera, its target, the physics and a clock. The clock stands at
// noon unless told otherwise, so that the frames of two runs are the same.
class Services final : public GameServices {
  public:
    static constexpr int64_t DayMs = 24ll*60*60*1000;

    WorldView*          view    = nullptr;
    Camera*             cam     = nullptr;
    const CameraTarget* target  = nullptr;
    DynamicWorld*       physics = nullptr;

    // Milliseconds of the day to start at, and how many of them pass per real millisecond (0: the time stands).
    void    setClock(int64_t dayMs, float pace);
    // Advances the island's time by dt real milliseconds.
    void    tick(uint64_t dt);

    ::World*                world()                   override { return nullptr; }
    WorldView*              worldView()         const override { return view; }
    const BloodDecalSource* bloodDecalSource()  const override { return nullptr; }
    Npc*                    player()                  override { return nullptr; }
    const CameraTarget*     cameraTarget()            override { return target; }
    Camera*                 camera()                  override { return cam; }
    void                    cameraProjection(Tempest::Matrix4x4&) override {}
    bool                    cameraShake(Tempest::Vec3&) override { return false; }
    bool                    isPause()           const override { return false; }

    int64_t                 worldTime()         const override;
    uint64_t                worldTickCount()    const override { return ticks; }
    DynamicWorld*           physicsWorld()      const override { return physics; }

    const VisualFx*         loadVisualFx(std::string_view) override { return nullptr; }
    const ParticleFx*       loadParticleFx(std::string_view, bool) override { return nullptr; }
    const ParticleFx*       loadParticleFx(const ParticleFx*, const VisualFx::Key*) override { return nullptr; }
    const Def::SoundEffect& soundEffect(std::string_view) const override { return sound; }
    const CameraPresets&    cameraDefinitions() const override { return cameras; }

  private:
    double           clock = 12.0*60*60*1000; // milliseconds of the day: noon, the sun high
    float            pace  = 0;
    uint64_t         ticks = 0;               // milliseconds since the start, for animated materials
    Def::SoundEffect sound{};
    CameraTable      cameras;
  };

// "12:00", "6:30" or "18" as milliseconds of the day; -1 if it is not a time.
int64_t parseDayTime(std::string_view text);

}
