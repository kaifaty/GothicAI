#include <Tempest/Window>
#include <Tempest/Application>
#include <Tempest/Log>


#include <Tempest/VulkanApi>

#if defined(_MSC_VER)
#include <Tempest/DirectX12Api>
#endif

#if defined(__APPLE__)
#include <Tempest/MetalApi>
#endif

#include "utils/crashlog.h"
#include "mainwindow.h"
#include "gothic.h"
#include "build.h"
#include "commandline.h"
#include "resources.h"
#include "aibridge.h"
#include "basestore.h"
#include "testbench.h"

#include <dmusic.h>

#include <cstdlib>
#include <string_view>

std::string_view selectDevice(const Tempest::AbstractGraphicsApi& api) {
  auto d = api.devices();

  static Tempest::Device::Props p;
  for(auto& i:d)
    // if(i.type==Tempest::DeviceType::Integrated) {
    if(i.type==Tempest::DeviceType::Discrete) {
      p = i;
      return p.name;
      }
  if(d.size()>0) {
    p = d[0];
    return p.name;
    }
  return "";
  }

std::unique_ptr<Tempest::AbstractGraphicsApi> mkApi(const CommandLine& g) {
  Tempest::ApiFlags flg = g.isValidationMode() ? Tempest::ApiFlags::Validation : Tempest::ApiFlags::NoFlags;
  switch(g.graphicsApi()) {
    case CommandLine::DirectX12:
#if defined(_MSC_VER)
      return std::make_unique<Tempest::DirectX12Api>(flg);
#else
      break;
#endif
    case CommandLine::Vulkan:
#if !defined(__APPLE__)
      return std::make_unique<Tempest::VulkanApi>(flg);
#else
      break;
#endif
    }

#if defined(__APPLE__)
  return std::make_unique<Tempest::MetalApi>(flg);
#else
  return std::make_unique<Tempest::VulkanApi>(flg);
#endif
  }

static bool envIs(const char* name, std::string_view value) {
  const char* env = std::getenv(name);
  return env!=nullptr && std::string_view(env)==value;
  }

// On by default; OPENGOTHIC_PIPELINE_CACHE=0 turns the on-disk pipeline cache off.
// It sits next to the other derived caches, but is keyed by GPU and driver, not by the game base.
// OPENGOTHIC_MSL_CACHE=0 turns off the SPIR-V -> MSL translation cache (Metal) on its own;
// OPENGOTHIC_MSL_CACHE_CHECK=1 translates every cached shader again and aborts on any difference.
static void setupPipelineCache() {
  try {
    const auto derived = BaseStore::cacheRoot()/"derived";
    if(!envIs("OPENGOTHIC_PIPELINE_CACHE","0"))
      Tempest::Device::setPipelineCacheDirectory(derived/"pipelines");
    if(!envIs("OPENGOTHIC_MSL_CACHE","0"))
      Tempest::Device::setShaderCacheDirectory(derived/"msl",envIs("OPENGOTHIC_MSL_CACHE_CHECK","1"));
    }
  catch(const std::exception& e) {
    Tempest::Log::i("pipeline cache: no cache directory (",e.what(),")");
    }
  }

int main(int argc,const char** argv) {
  TestBench::install();
  try {
    static Tempest::WFile logFile("log.txt");
    Tempest::Log::setOutputCallback([](Tempest::Log::Mode mode, const char* text) {
      logFile.write(text,std::strlen(text));
      logFile.write("\n",1);
      if(mode==Tempest::Log::Error)
        logFile.flush();
      });
    }
  catch(...) {
    Tempest::Log::e("unable to setup logfile - fallback to console log");
    }
  CrashLog::setup();

  daedalus::set_log_level(daedalus::LogLevel::INFO);
  Dm_setLogger(DmLogLevel_INFO, [](void* ctx, DmLogLevel lvl, char const* msg) {
    switch (lvl) {
      case DmLogLevel_FATAL:
      case DmLogLevel_ERROR:
      case DmLogLevel_WARN:
        Tempest::Log::e("[dmusic] ", msg);
        break;
      case DmLogLevel_INFO:
        Tempest::Log::i("[dmusic] ", msg);
        break;
      case DmLogLevel_DEBUG:
      case DmLogLevel_TRACE:
        Tempest::Log::d("[dmusic] ", msg);
        break;
      }
    }, nullptr);

  Tempest::Log::i(appBuild);
  Workers::setThreadName("Main thread");

  CommandLine          cmd{argc,argv};
  auto                 api     = mkApi(cmd);
  const auto           gpuName = selectDevice(*api);
  CrashLog::setGpu(gpuName);

  setupPipelineCache();
  Tempest::Device      device{*api,gpuName};
  CrashLog::setGpu(device.properties().name);

  Tempest::Application app;
  const auto startup=Tempest::Application::tickCount();
  try {
  Resources            resources{device};
  Gothic               gothic;
  GameMusic            music;
  gothic.setupGlobalScripts();
  AiBridge             aiBridge;

  MainWindow           wx(device);
  Tempest::Log::i("base: game window ready (",Tempest::Application::tickCount()-startup," ms since base preparation)");
  return app.exec();
  }
  catch(const BaseStore::Error& error) {
    Tempest::Log::e(error.what());
    return EXIT_FAILURE;
    }
  }
