#pragma once

// What a program of the platform alone gives the engine in place of Gothic (engine layers, step 8): the services the
// renderer asks for, without an installation, Gothic.ini, a base, scripts or the Gothic object. The render test
// (platform_draw_test.cpp) draws with them; the island program (step 8.6) takes them as its pattern. Only platform
// headers here (scripts/engine_layers.py layer_of), so a program that copies them stays on the platform.
//
// The order they need each other in:
//   Arguments  before Resources: Resources reads the command line in its constructor (hero heads);
//   Services   after Resources has its device: Settings and Shaders read the device's features.

#include "camerapresets.h"
#include "commandline.h"
#include "gameservices.h"
#include "settings.h"
#include "graphics/shaders.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace platform {

// The command line of a program without an installation: -g names an empty folder, so InstallDetect never runs.
// Render options (bindless, ray query, VSM) stay at their defaults for the device.
class Arguments final {
  public:
    explicit Arguments(const std::filesystem::path& root)
      : path(root.string()), argv{"platform", "-g", path.c_str()}, cmd(int(argv.size()), argv.data()) {}

  private:
    std::string                path;
    std::array<const char*,3>  argv;
    CommandLine                cmd;
  };

// One camera preset for every mode: Camera reads a table, a program of the platform has no camera modes of its own.
// Metres and degrees as in the game's presets: a third-person camera 3 m behind, 30 degrees above.
class CameraTable final : public CameraPresets {
  public:
    CameraTable() {
      cam.best_range     = 3;
      cam.min_range      = 2;
      cam.max_range      = 10;
      cam.best_elevation = 30;
      cam.max_elevation  = 90;
      cam.velo_trans     = 40;
      cam.velo_rot       = 2;
      cam.translate      = 1;
      cam.rotate         = 1;
      cam.collision      = 1;
      }

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

// The game's side of the renderer: a view, a camera and a clock. No world, player, physics, effects or sounds.
class Game final : public GameServices {
  public:
    WorldView* view  = nullptr;
    Camera*    cam   = nullptr;
    int64_t    time  = 12*60*60*1000; // milliseconds of the day: noon, the sun high
    uint64_t   ticks = 0;             // milliseconds since the start, for animated materials and particles

    World*                  world()                   override { return nullptr; }
    WorldView*              worldView()         const override { return view; }
    const BloodDecalSource* bloodDecalSource()  const override { return nullptr; }
    Npc*                    player()                  override { return nullptr; }
    const CameraTarget*     cameraTarget()            override { return nullptr; }
    Camera*                 camera()                  override { return cam; }
    void                    cameraProjection(Tempest::Matrix4x4&) override {}
    bool                    cameraShake(Tempest::Vec3&) override { return false; }
    bool                    isPause()           const override { return false; }

    int64_t                 worldTime()         const override { return time; }
    uint64_t                worldTickCount()    const override { return ticks; }
    DynamicWorld*           physicsWorld()      const override { return nullptr; }

    const VisualFx*         loadVisualFx(std::string_view) override { return nullptr; }
    const ParticleFx*       loadParticleFx(std::string_view, bool) override { return nullptr; }
    const ParticleFx*       loadParticleFx(const ParticleFx*, const VisualFx::Key*) override { return nullptr; }
    const Def::SoundEffect& soundEffect(std::string_view) const override { return sound; }
    const CameraPresets&    cameraDefinitions() const override { return cameras; }

  private:
    Def::SoundEffect sound{};
    CameraTable      cameras;
  };

// Settings with the engine's defaults only: both ini files would lie in the empty folder, so none is read (and none
// is written: nothing calls Settings::flush).
class Options final {
  public:
    explicit Options(const std::filesystem::path& root) {
      settings.load((root/"system.ini").u16string(),(root/"user.ini").u16string());
      settings.setupOptions();
      settings.refreshPhysicsFlags();
      }

  private:
    Settings settings;
  };

// Everything the renderer asks of a game once Resources has its device. The shaders compile in the background;
// Renderer::draw waits for them.
struct Services final {
  explicit Services(const std::filesystem::path& root) : options(root) {}

  Options options;
  Game    game;
  Shaders shaders;
  };

}
