#pragma once

#include <Tempest/Platform>
#include <Tempest/Dir>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "game/constants.h"

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
    bool                doStartMenu()      const { return !noMenu;      }
    bool                doIntro()          const { return !noIntro;     }
    bool                doHeroHead()       const { return !noHeroHead;  }
    bool                doRenderRig()      const { return !noRenderRig; }
    Benchmark           isBenchmarkMode()  const { return isBenchmark;  }
    uint32_t            aaPreset()         const { return aaPresetId;   }
    bool                doTaa()            const { return isTaa;        }
    bool                doEye()            const { return isEye;        }
    int                 motionVectors()    const { return isMv;         }
    bool                doHashedAlpha()    const { return isHashAlpha;  }
    std::string_view    defaultSave()      const { return saveDef;    }

    std::string         wrldDef;

  private:
    GraphicBackend      graphics = GraphicBackend::Vulkan;
    std::u16string      gpath, gmod;
    std::string         saveDef;
    bool                devmode      = false;
    bool                noMenu       = false;
    bool                noIntro      = false;  // -nointro: no videos, the start's dialog counts as told
    bool                noHeroHead   = false;
    bool                noRenderRig  = false;  // --no-render-rig: bodies on Gothic's skeleton (graphics/mesh/renderrig)
    Benchmark           isBenchmark  = Benchmark::None;
    bool                isWindow     = false;
    bool                isDebug      = false;
    // ray tracing is off everywhere, -rt 1 turns it on: the scene rebuilt the whole TLAS
    // whenever an object moved, 4-8 FPS indoors on an M1 (docs/performance.md, 2026-10-28)
    bool                isRQuery     = false;
#if defined(__OSX__)
    bool                isMeshSh     = false;  // slower on M1 (reports/render-features/2026-10-18)
#else
    bool                isMeshSh     = true;
#endif
    bool                isBindlessSh = true;
#if defined(__APPLE__)
    // virtual shadow maps: off on Metal (M1 Pro: +6.5..21 ms GPU at 2560x1440 without the static cache), -vsm 1 enables
    bool                isVsm        = false;
#else
    bool                isVsm        = true;  // virtual shadow maps: sun and lamp shadows, where the GPU can (Shaders::isVsmSupported)
#endif
    GiMethod            isGi         = GiMethod::None;
    bool                isGiSet      = false;  // -gi given: else Gothic picks by the device
    uint32_t            aaPresetId = 1;        // CMAA2; -aa 0 turns it off
    bool                isTaa      = true;     // temporal AA (replaces CMAA2); -taa 0 turns it off
    bool                isEye      = true;     // the eye's adaptation under -gi 1 (graphics/eyeadaptation.h); -eye 0 turns it off
    int                 isMv       = -1;       // motion vectors of objects: -mv 0|1; -1 unset, the ini decides
    bool                isHashAlpha = true;    // hashed alpha of our glTF MASK under TAA (BK_HASHED_ALPHA); -hashalpha 0: threshold 0.5
  };

