#pragma once

#include <Tempest/Platform>
#include <Tempest/Dir>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "game/constants.h"

class VersionInfo;
class GothicNotFoundException : std::logic_error {
  using logic_error::logic_error;
  };

class CommandLine {
  public:
    CommandLine(int argc,const char** argv);
    static const CommandLine& inst();

    enum GraphicBackend : uint8_t {
      Vulkan,
      DirectX12
      };
    auto                graphicsApi() const -> GraphicBackend;
    std::u16string_view rootPath() const;
    std::u16string_view modPath() const { return gmod; }
    std::u16string      nestedPath(const std::initializer_list<const char16_t*> &name, Tempest::Dir::FileType type) const;

    bool                isDevMode()        const { return devmode;      }
    bool                isValidationMode() const { return isDebug;      }
    bool                isWindowMode()     const { return isWindow;     }
    bool                isRayQuery()       const { return isRQuery;     }
    GiMethod            isRtGi()           const { return isGi;         }
    bool                hasRtGiArg()       const { return isGiSet;      }
    bool                isMeshShading()    const { return isMeshSh;     }
    bool                isBindless()       const { return isBindlessSh; }
    bool                isVirtualShadow()  const { return isVsm;        }
    bool                isSoftwareShadow() const { return isRtSm;       }
    bool                doStartMenu()      const { return !noMenu;      }
    bool                doIntro()          const { return !noIntro;     }
    Benchmark           isBenchmarkMode()  const { return isBenchmark;  }
    bool                doForceG1()        const { return forceG1;      }
    bool                doForceG2()        const { return forceG2;      }
    bool                doForceG2NR()      const { return forceG2NR;    }
    uint32_t            aaPreset()         const { return aaPresetId;   }
    std::string_view    defaultSave()      const { return saveDef;    }

    std::string         wrldDef;

  private:
    GraphicBackend      graphics = GraphicBackend::Vulkan;
    std::u16string      gpath, gmod;
    std::string         saveDef;
    bool                devmode      = false;
    bool                noMenu       = false;
    bool                noIntro      = false;  // -nointro: no videos, the start's dialog counts as told
    Benchmark           isBenchmark  = Benchmark::None;
    bool                isWindow     = false;
    bool                isDebug      = false;
    // ray tracing is off everywhere, -rt 1 turns it on: the scene rebuilt the whole TLAS
    // whenever an object moved, 4-8 FPS indoors on an M1 (docs/performance.md, 2026-10-28)
    bool                isRQuery     = false;
#if defined(__ANDROID__)
    bool                isMeshSh     = false;
#elif defined(__OSX__)
    bool                isMeshSh     = false;  // slower on M1 (reports/render-features/2026-10-18)
#else
    bool                isMeshSh     = true;
#endif
    bool                isBindlessSh = true;
    bool                isVsm        = false;
    bool                isRtSm       = false;
    GiMethod            isGi         = GiMethod::None;
    bool                isGiSet      = false;  // -gi given: else Gothic picks by the device
    bool                forceG1      = false;
    bool                forceG2      = false;
    bool                forceG2NR    = false;
    uint32_t            aaPresetId = 1;        // CMAA2; -aa 0 turns it off
  };

