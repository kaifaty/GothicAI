#pragma once

#include "game/definitions/definitiontypes.h"

class CameraDefinitions final {
  public:
    CameraDefinitions();

    const Def::Camera& dialogCam()    const { return camModDialog;    }
    const Def::Camera& inventoryCam() const { return camModInventory; }
    const Def::Camera& stdCam()       const { return camModNormal; }
    const Def::Camera& fpCam()        const { return camModFp;     }
    const Def::Camera& backCam()      const { return camModBack;   }
    const Def::Camera& meleeCam()     const { return camModMelee;  }
    const Def::Camera& rangeCam()     const { return camModRange;  }
    const Def::Camera& mageCam()      const { return camModMage;   }
    const Def::Camera& deathCam()     const { return camModDeath;  }
    const Def::Camera& swimCam()      const { return camModSwim;   }
    const Def::Camera& diveCam()      const { return camModDive;   }
    const Def::Camera& fallCam()      const { return camModFall;   }
    const Def::Camera& mobsiCam(std::string_view tag, std::string_view pos) const;

  private:
    Def::Camera getCam(std::string_view name);

    std::vector<std::pair<std::string, Def::Camera>> cameras;
    const Def::Camera* find(std::string_view name) const;

    Def::Camera camModNormal, camModFp, camModBack;
    Def::Camera camModDialog, camModInventory, camModDeath, camModSwim, camModDive, camModFall;
    Def::Camera camModMelee, camModRange, camModMage;
  };
