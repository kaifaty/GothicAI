#include "definitiontypes.h"

#include <algorithm>
#include <iterator>
#include <type_traits>

namespace {
template<class T>
void copyField(T& to, const T& from) {
  to = from;
  }

template<class T, size_t N>
void copyField(T (&to)[N], const T (&from)[N]) {
  std::copy(std::begin(from), std::end(from), std::begin(to));
  }
}

namespace Def {

static_assert(sizeof(FightMove)==4 && sizeof(MusicTransitionEffect)==4 && sizeof(MusicTransitionType)==4);
static_assert(uint32_t(FightMove::NOP)==0 && uint32_t(FightMove::RUN)==1 && uint32_t(FightMove::RUN_BACK)==2 &&
              uint32_t(FightMove::JUMP_BACK)==3 && uint32_t(FightMove::TURN)==4 && uint32_t(FightMove::STRAFE)==5 &&
              uint32_t(FightMove::ATTACK)==6 && uint32_t(FightMove::ATTACK_SIDE)==7 &&
              uint32_t(FightMove::ATTACK_FRONT)==8 && uint32_t(FightMove::ATTACK_TRIPLE)==9 &&
              uint32_t(FightMove::ATTACK_WHIRL)==10 && uint32_t(FightMove::ATTACK_MASTER)==11 &&
              uint32_t(FightMove::TURN_TO_HIT)==15 && uint32_t(FightMove::PARRY)==17 &&
              uint32_t(FightMove::STAND_UP)==18 && uint32_t(FightMove::WAIT)==19 &&
              uint32_t(FightMove::WAIT_LONGER)==23 && uint32_t(FightMove::WAIT_EXT)==24);
static_assert(uint32_t(MusicTransitionEffect::UNKNOWN)==0 && uint32_t(MusicTransitionEffect::NONE)==1 &&
              uint32_t(MusicTransitionEffect::GROOVE)==2 && uint32_t(MusicTransitionEffect::FILL)==3 &&
              uint32_t(MusicTransitionEffect::BREAK)==4 && uint32_t(MusicTransitionEffect::INTRO)==5 &&
              uint32_t(MusicTransitionEffect::END)==6 && uint32_t(MusicTransitionEffect::END_AND_INTO)==7);
static_assert(uint32_t(MusicTransitionType::UNKNOWN)==0 && uint32_t(MusicTransitionType::IMMEDIATE)==1 &&
              uint32_t(MusicTransitionType::BEAT)==2 && uint32_t(MusicTransitionType::MEASURE)==3);
static_assert(FightAi::move_count==6 && EffectBase::user_string_count==5);
static_assert(std::is_same_v<decltype(FightAi::move_count),const uint32_t> &&
              std::is_same_v<decltype(EffectBase::user_string_count),const uint8_t>);

}

namespace VmDef {

void Camera::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("CCAMSYS." symbol, &Camera::name);
  DEF_CAMERA_FIELDS(DEF_REGISTER_FIELD)
#undef DEF_REGISTER_FIELD
  }

Def::Camera Camera::copy() const {
  Def::Camera result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_CAMERA_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

void SoundEffect::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("C_SFX." symbol, &SoundEffect::name);
  DEF_SOUND_EFFECT_FIELDS(DEF_REGISTER_FIELD)
#undef DEF_REGISTER_FIELD
  }

Def::SoundEffect SoundEffect::copy() const {
  Def::SoundEffect result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_SOUND_EFFECT_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

void MusicTheme::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("C_MUSICTHEME." symbol, &MusicTheme::name);
  DEF_MUSIC_THEME_FIELDS(DEF_REGISTER_FIELD)
#undef DEF_REGISTER_FIELD
  }

Def::MusicTheme MusicTheme::copy() const {
  Def::MusicTheme result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_MUSIC_THEME_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

void ParticleEffect::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("C_PARTICLEFX." symbol, &ParticleEffect::name);
  DEF_PARTICLE_EFFECT_COMMON_FIELDS(DEF_REGISTER_FIELD)
  // Gothic 2 only
  if(s.find_symbol_by_name("C_PARTICLEFX.FLOCKMODE") != nullptr) {
    DEF_PARTICLE_EFFECT_G2_FIELDS(DEF_REGISTER_FIELD)
    }
#undef DEF_REGISTER_FIELD
  }

Def::ParticleEffect ParticleEffect::copy() const {
  Def::ParticleEffect result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_PARTICLE_EFFECT_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

void EffectBase::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("CFX_BASE." symbol, &EffectBase::name);
  DEF_EFFECT_BASE_COMMON_FIELDS(DEF_REGISTER_FIELD)
  // Gothic 2 only
  if(s.find_symbol_by_name("CFX_BASE.EMFXCOLLDYNPERC_S") != nullptr) {
    DEF_EFFECT_BASE_G2_FIELDS(DEF_REGISTER_FIELD)
    }
#undef DEF_REGISTER_FIELD
  }

Def::EffectBase EffectBase::copy() const {
  Def::EffectBase result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_EFFECT_BASE_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

void ParticleEffectEmitKey::register_(daedalus::DaedalusScript& s) {
#define DEF_REGISTER_FIELD(type, name, extent, symbol) s.register_member("C_PARTICLEFXEMITKEY." symbol, &ParticleEffectEmitKey::name);
  DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(DEF_REGISTER_FIELD)
#undef DEF_REGISTER_FIELD
  }

Def::ParticleEffectEmitKey ParticleEffectEmitKey::copy() const {
  Def::ParticleEffectEmitKey result{};
#define DEF_COPY_FIELD(type, name, extent, symbol) copyField(result.name, name);
  DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(DEF_COPY_FIELD)
#undef DEF_COPY_FIELD
  return result;
  }

}
