// engine-island: the empty island of the platform alone (engine layers, step 8.6:
// reports/engine/2026-11-13-empty-island/README.md). Its own window, a capsule that walks and jumps over the island
// that island-data wrote, the chunks streamed as the hero goes. No Gothic, no base, no installation, no sound; the
// island's folders are set here, not by the environment. engine-island --help lists the flags.

#include "islandflags.h"
#include "islandhero.h"
#include "islandservices.h"
#include "islandview.h"
#include "islandwindow.h"
#include "islandworld.h"

#include "resources.h"
#include "graphics/shaders.h"

#include <Tempest/Application>
#include <Tempest/Device>
#include <Tempest/Log>
#if defined(__APPLE__)
#include <Tempest/MetalApi>
#else
#include <Tempest/VulkanApi>
#endif

#include <cstdio>
#include <exception>

int main(int argc, const char** argv) {
  island::Flags flags;
  flags.data = ISLAND_DATA_DIR;
  std::string error;
  if(!island::parseFlags(argc, argv, flags, error)) {
    std::fprintf(stderr, "engine-island: %s\n%s", error.c_str(), island::usage());
    return 2;
    }
  if(flags.help) {
    std::printf("%s", island::usage());
    return 0;
    }
  try {
    const island::DataDir data{std::filesystem::absolute(flags.data)};
    island::useIslandFolders(data);
    island::Arguments arguments(data.root);

#if defined(__APPLE__)
    Tempest::MetalApi  api;
#else
    Tempest::VulkanApi api;
#endif
    Tempest::Device      device(api);
    Tempest::Application app;
    Resources            resources(device, Resources::Audio::Off);
    island::Options      options(data.root);
    island::Services     services;
    services.setClock(flags.dayTime, flags.pace);
    Shaders              shaders;

    island::World world(data);
    island::View  view(world);
    const auto&   start = world.start();
    island::Hero  hero(world.physics(), start.feet+Tempest::Vec3(0, 5, 0), start.yaw);
    services.physics = &world.physics();
    services.view    = &view.view();

    int code = 0;
    {
    island::Window window(device, flags, services, world, view, hero);
    app.exec();
    code = window.exitCode();
    }
    services.target = nullptr;
    services.cam    = nullptr;
    return code;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr, "engine-island: %s\n", e.what());
    return 1;
    }
  }
