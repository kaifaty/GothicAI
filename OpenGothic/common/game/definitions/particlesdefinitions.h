#pragma once

#include <zenkit/DaedalusVm.hh>
#include <zenkit/addon/daedalus.hh>
#include "game/definitions/definitiontypes.h"

#include <unordered_map>
#include <memory>
#include <mutex>

#include "graphics/visualfx.h"

class ParticleFx;

// The aimed stream (judged on bench stills, scripts/bench/humiliate-look.bench): the native PEE's gravity
// 0.002 cm/ms^2; velocity in cm/ms, angles in degrees in the npc's frame at its pelvis (head 0: ahead): it
// lands about 85 cm ahead at pelvis height, on the chest of the kneeling hero
namespace PeeStream {
  inline constexpr float pps        = 500;
  inline constexpr float head       = 0;
  inline constexpr float elev       = 20;
  inline constexpr float spread     = 0.6f;
  inline constexpr float vel        = 0.5f;
  inline constexpr float velSpread  = 0.03f;
  inline constexpr float lifeMs     = 220;
  inline constexpr float alphaStart = 255;
  inline constexpr float alphaEnd   = 180;
  inline constexpr char  size[]     = "1.5 30";
  inline constexpr float sizeEnd    = 1.0f;
  inline constexpr char  texture[]  = "PARTYEL1.TGA";  // PEE's own drop, drawn narrow and long: a wide one reads as leaves
  inline constexpr char  color[]    = "255 255 255";
  }

class ParticlesDefinitions final {
  public:
    ParticlesDefinitions();
    ~ParticlesDefinitions();

    const ParticleFx* get(std::string_view name, bool relaxed);
    const ParticleFx* get(const ParticleFx* base, const VisualFx::Key* key);

  private:
    std::recursive_mutex                sync;
    std::unique_ptr<zenkit::DaedalusVm> vm;

    std::unordered_map<std::string,          std::unique_ptr<ParticleFx>> pfx;
    std::unordered_map<const VisualFx::Key*, std::unique_ptr<ParticleFx>> pfxKey;

    const ParticleFx* implGet(std::string_view name, bool relaxed);
    const ParticleFx* implGet(const ParticleFx& base, const VisualFx::Key& key);

    std::shared_ptr<Def::ParticleEffect> implGetDirect(std::string_view name, bool relaxed);
    std::shared_ptr<Def::ParticleEffect> peeStream();
  };
