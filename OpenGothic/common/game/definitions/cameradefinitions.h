#pragma once

#include "camerapresets.h"

#include <vector>

class CameraDefinitions final : public CameraPresets {
  public:
    CameraDefinitions();

    const Def::Camera& dialogCam()    const override { return camModDialog;    }
    const Def::Camera& inventoryCam() const override { return camModInventory; }
    const Def::Camera& stdCam()       const override { return camModNormal; }
    const Def::Camera& fpCam()        const override { return camModFp;     }
    const Def::Camera& backCam()      const override { return camModBack;   }
    const Def::Camera& meleeCam()     const override { return camModMelee;  }
    const Def::Camera& rangeCam()     const override { return camModRange;  }
    const Def::Camera& mageCam()      const override { return camModMage;   }
    const Def::Camera& deathCam()     const override { return camModDeath;  }
    const Def::Camera& swimCam()      const override { return camModSwim;   }
    const Def::Camera& diveCam()      const override { return camModDive;   }
    const Def::Camera& fallCam()      const override { return camModFall;   }
    const Def::Camera& mobsiCam(std::string_view tag, std::string_view pos) const override;

  private:
    Def::Camera getCam(std::string_view name);

    std::vector<std::pair<std::string, Def::Camera>> cameras;
    const Def::Camera* find(std::string_view name) const;

    Def::Camera camModNormal, camModFp, camModBack;
    Def::Camera camModDialog, camModInventory, camModDeath, camModSwim, camModDive, camModFall;
    Def::Camera camModMelee, camModRange, camModMage;
  };
