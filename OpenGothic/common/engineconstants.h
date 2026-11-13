#pragma once

#include <cstdint>

enum BodyState:uint32_t {
  BS_NONE                  = 0,

  BS_MOD_HIDDEN            = 1 << 7,
  BS_MOD_DRUNK             = 1 << 8,
  BS_MOD_NUTS              = 1 << 9,
  BS_MOD_BURNING           = 1 << 10,
  BS_MOD_CONTROLLED        = 1 << 11,
  BS_MOD_TRANSFORMED       = 1 << 12,
  BS_FLAG_INTERRUPTABLE    = 1 << 15,
  BS_FLAG_FREEHANDS        = 1 << 16,

  BS_STAND                 = BS_FLAG_INTERRUPTABLE | BS_FLAG_FREEHANDS,
  BS_MOD_MASK              = BS_MOD_HIDDEN | BS_MOD_DRUNK | BS_MOD_NUTS | BS_MOD_BURNING | BS_MOD_CONTROLLED,
  BS_FLAG_MASK             = BS_FLAG_INTERRUPTABLE | BS_FLAG_FREEHANDS,

  BS_WALK                  = 1  | BS_FLAG_INTERRUPTABLE,
  BS_SNEAK                 = 2  | BS_FLAG_INTERRUPTABLE,
  BS_RUN                   = 3,
  BS_SPRINT                = 4,
  BS_SWIM                  = 5,
  BS_CRAWL                 = 6,
  BS_DIVE                  = 7,
  BS_JUMP                  = 8,
  BS_CLIMB                 = 9  | BS_FLAG_INTERRUPTABLE,
  BS_FALL                  = 10,
  BS_SIT                   = 11 | BS_FLAG_FREEHANDS,
  BS_LIE                   = 12,
  BS_INVENTORY             = 13,
  BS_ITEMINTERACT          = 14 | BS_FLAG_INTERRUPTABLE,
  BS_MOBINTERACT           = 15,
  BS_MOBINTERACT_INTERRUPT = 16 | BS_FLAG_INTERRUPTABLE,
  BS_TAKEITEM              = 17,
  BS_DROPITEM              = 18,
  BS_THROWITEM             = 19,
  BS_PICKPOCKET            = 20 | BS_FLAG_INTERRUPTABLE,
  BS_STUMBLE               = 21,
  BS_UNCONSCIOUS           = 22,
  BS_DEAD                  = 23,
  BS_AIMNEAR               = 24,
  BS_AIMFAR                = 25,
  BS_HIT                   = 26 | BS_FLAG_INTERRUPTABLE,
  BS_PARADE                = 27,
  BS_CASTING               = 28,
  BS_PETRIFIED             = 29,
  BS_CONTROLLING           = 30 | BS_FLAG_INTERRUPTABLE,
  BS_MAX                   = 31,

  BS_MAX_FLAGS             = BS_MAX | BS_MOD_MASK | BS_FLAG_MASK,
  };

enum class WeaponState : uint8_t {
  NoWeapon,
  Fist,
  W1H,
  W2H,
  Bow,
  CBow,
  Mage
  };

enum class WalkBit : uint8_t {
  WM_Run  =0,
  WM_Walk =1,
  WM_Sneak=2,
  WM_Water=4,
  WM_Swim =8,
  WM_Dive =16,
  };

inline WalkBit operator & (WalkBit a,const WalkBit& b){
  return WalkBit(uint8_t(a)&uint8_t(b));
  }

inline WalkBit operator | (WalkBit a,const WalkBit& b){
  return WalkBit(uint8_t(a)|uint8_t(b));
  }

inline WalkBit operator ^ (WalkBit a,const WalkBit& b){
  return WalkBit(uint8_t(a)^uint8_t(b));
  }

inline WalkBit operator ~ (WalkBit a){
  return WalkBit(~uint8_t(a));
  }

enum class AaPreset : uint32_t {
  OFF,
  MEDIUM,
  ULTRA,
  PRESETS_COUNT
  };

enum class Benchmark : uint8_t {
  None,
  Normal,
  CiTooling
  };

enum GiMethod : uint8_t {
  None,
  Probes, // legacy
  IrrC,
  Count
  };
