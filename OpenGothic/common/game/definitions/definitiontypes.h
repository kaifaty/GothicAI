#pragma once

#include "records/definitions.h"
#include "daedalus/script.h"

namespace VmDef {

// Members must be declared on the bound class itself for the VM's type checks.
#define DEF_DECLARE_FIELD(type, name, extent, symbol) type name extent;

struct Camera final : daedalus::DaedalusInstance {
  DEF_CAMERA_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::Camera copy() const;
  };

struct SoundEffect final : daedalus::DaedalusInstance {
  DEF_SOUND_EFFECT_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::SoundEffect copy() const;
  };

struct MusicTheme final : daedalus::DaedalusInstance {
  DEF_MUSIC_THEME_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::MusicTheme copy() const;
  };

struct ParticleEffect final : daedalus::DaedalusInstance {
  DEF_PARTICLE_EFFECT_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::ParticleEffect copy() const;
  };

struct EffectBase final : daedalus::DaedalusInstance {
  static constexpr uint8_t user_string_count = Def::EffectBase::user_string_count;

  DEF_EFFECT_BASE_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::EffectBase copy() const;
  };

struct ParticleEffectEmitKey final : daedalus::DaedalusInstance {
  DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(DEF_DECLARE_FIELD)

  static void register_(daedalus::DaedalusScript& s);
  Def::ParticleEffectEmitKey copy() const;
  };

#undef DEF_DECLARE_FIELD

}
