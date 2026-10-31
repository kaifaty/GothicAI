#pragma once

#include <Tempest/SoundEffect>
#include <Tempest/Sound>
#include <vector>

#include "game/definitions/definitiontypes.h"

class GSoundEffect;

class SoundFx {
  public:
    SoundFx(std::string_view tagname);
    SoundFx(Tempest::Sound &&raw);
    SoundFx(SoundFx&&)=default;
    SoundFx& operator=(SoundFx&&)=default;

    struct Sample {
      const Tempest::Sound* sound = nullptr;
      float                 volume = 0.f;
      bool                  loop = false;
      };

    // A silent simulation still selects the native variant (including its RNG draw).
    Sample selectSample() const;
    Tempest::SoundEffect load(Tempest::SoundDevice& dev, bool& loop) const;

  private:
    struct SoundVar {
      SoundVar()=default;
      SoundVar(const Def::SoundEffect& sfx,Tempest::Sound&& snd);
      SoundVar(const float vol,Tempest::Sound&& snd);

      Tempest::Sound snd;
      float          vol  = 0.5f;
      bool           loop = false;
      };

    std::vector<SoundVar> inst;
    std::string           debugName;
    void implLoad    (std::string_view name);
    void loadVariants(std::string_view name);
  };

