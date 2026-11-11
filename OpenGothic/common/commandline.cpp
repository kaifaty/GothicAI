#include "commandline.h"

#include <Tempest/Log>
#include <Tempest/TextCodec>
#include <cstdlib>
#include <cstring>
#include <cassert>

#if defined(__APPLE__)
#include <filesystem>
#endif

#include <algorithm>

#include "utils/installdetect.h"
#include "utils/fileutil.h"
#include "utils/string_frm.h"

using namespace Tempest;
using namespace FileUtil;

static CommandLine* instance = nullptr;

static bool boolArg(std::string_view v) {
  return std::string_view(v)!="0" && std::string_view(v)!="false";
  }

static int intArg(std::string_view v) {
  if(v=="false")
    return 0;
  return (std::stoi(std::string(v)));
  }

CommandLine::CommandLine(int argc, const char** argv) {
  instance = this;
  // OPENGOTHIC_VSM=0|1: the default of -vsm for benches and scripts that cannot pass flags; -vsm wins
  if(const char* vsm = std::getenv("OPENGOTHIC_VSM"))
    isVsm = boolArg(vsm);
  // OPENGOTHIC_MS=0|1: the default of -ms (mesh shaders) for benches; -ms wins
  if(const char* ms = std::getenv("OPENGOTHIC_MS"))
    isMeshSh = boolArg(ms);
  // OPENGOTHIC_RT=0|1 and OPENGOTHIC_GI=0|1|2: the same for -rt and -gi (the GI clips of a bench); the flags win
  if(const char* rt = std::getenv("OPENGOTHIC_RT"))
    isRQuery = boolArg(rt);
  if(const char* gi = std::getenv("OPENGOTHIC_GI"); gi!=nullptr && *gi!='\0') {
    isGi    = GiMethod(std::clamp(std::max(0, intArg(gi)), 0, GiMethod::Count-1));
    isGiSet = true;
    }
  if(argc<1)
    return;

  std::string_view mod;
  for(int i=1;i<argc;++i) {
    std::string_view arg = argv[i];
    if(arg.find("-game:")==0) {
      if(!mod.empty())
        Log::e("-game specified twice");
      mod = arg.substr(6);
      }
    else if(arg=="-g") {
      ++i;
      if(i<argc)
        gpath.assign(argv[i],argv[i]+std::strlen(argv[i]));
      }
    else if(arg=="-devmode") {
      // http://www.gothic-library.ru/publ/marvin/1-1-0-547
      devmode = true;
      }
    else if(arg=="-save") {
      ++i;
      if(i<argc){
        if(std::strcmp(argv[i],"q")==0) {
          saveDef = "save_slot_0.sav";
          } else {
          saveDef = string_frm("save_slot_",argv[i],".sav");
          }
        }
      }
    else if(arg=="-w") {
      ++i;
      if(i<argc)
        wrldDef = argv[i];
      }
    else if(arg=="-window") {
      isWindow = true;
      }
    else if(arg=="-nomenu") {
      noMenu = true;
      }
    else if(arg=="-nointro") {
      noIntro = true;
      }
    else if(arg=="--no-hero-head") {
      noHeroHead = true;
      }
    else if(arg=="--no-render-rig") {
      noRenderRig = true;
      }
    else if(arg=="-benchmark") {
      isBenchmark = Benchmark::Normal;
      if(i+1<argc && argv[i+1][0]!='-') {
        ++i;
        isBenchmark = std::string_view(argv[i])=="ci" ? Benchmark::CiTooling : isBenchmark;
        }
      }
    else if(arg=="-dx12") {
      graphics = GraphicBackend::DirectX12;
      }
    else if(arg=="-validation" || arg=="-v") {
      isDebug  = true;
      }
    else if(arg=="-rt") {
      ++i;
      if(i<argc)
        isRQuery = boolArg(argv[i]);
      }
    else if(arg=="-taa") {
      ++i;
      if(i<argc)
        isTaa = boolArg(argv[i]);
      }
    else if(arg=="-mv") {
      ++i;
      if(i<argc)
        isMv = boolArg(argv[i]) ? 1 : 0;
      }
    else if(arg=="-aa") {
      ++i;
      if(i<argc) {
        try {
          const int arg = std::max(0, intArg(argv[i]));
          aaPresetId = std::clamp(uint32_t(arg), 0u, uint32_t(AaPreset::PRESETS_COUNT)-1u);
          }
        catch (const std::exception& e) {
          Log::i("failed to read cmaa2 preset: \"", std::string(argv[i]), "\"");
          }
        }
      }
    else if(arg=="-gi") {
      ++i;
      if(i<argc) {
        const int arg = std::max(0, intArg(argv[i]));
        isGi    = GiMethod(std::clamp(arg, 0, GiMethod::Count-1));
        isGiSet = true;
        }
      }
    else if(arg=="-ms") {
      ++i;
      if(i<argc)
        isMeshSh = boolArg(argv[i]);
      }
    else if(arg=="-bl") {
      // not to document - debug only
      ++i;
      if(i<argc)
        isBindlessSh = boolArg(argv[i]);
      }
    else if(arg=="-vsm") {
      // virtual shadow maps for the sun and the lamps: on by default, off on Metal (commandline.h); -vsm 0|1
      ++i;
      if(i<argc)
        isVsm = boolArg(argv[i]);
      }
    else {
      Log::i("unrecognized commandline option: \"", arg, "\"");
      }
    }

  if(gpath.empty()) {
    InstallDetect inst;
    gpath = inst.detectG2();
#if defined(__APPLE__)
    if(!gpath.empty() && gpath==inst.applicationSupportDirectory()) {
      std::filesystem::current_path(gpath);
      }
#endif
    }

  for(auto& i:gpath)
    if(i=='\\')
      i='/';

  if(gpath.size()>0 && gpath.back()!='/')
    gpath.push_back('/');

  gmod    = TextCodec::toUtf16(mod);
  if(!gmod.empty())
    gmod = nestedPath({u"system",gmod.c_str()},Dir::FT_File);
  }

const CommandLine& CommandLine::inst() {
  assert(instance!=nullptr);
  return *instance;
  }

CommandLine::GraphicBackend CommandLine::graphicsApi() const {
  return graphics;
  }

std::u16string_view CommandLine::rootPath() const {
  return gpath;
  }

std::u16string CommandLine::nestedPath(const std::initializer_list<const char16_t*>& name, Tempest::Dir::FileType type) const {
  return FileUtil::nestedPath(gpath, name, type);
  }
