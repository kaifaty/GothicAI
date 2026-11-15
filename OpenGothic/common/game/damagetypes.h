#pragma once

#include "world/records/npcrecord.h"

#include <cstddef>

struct DamageValue final {
  DamageValue()=default;
  DamageValue(int32_t v,bool b):value(v),hasHit(b){}
  DamageValue(int32_t v,bool b,bool i):value(v),hasHit(b),invincible(i){}

  int32_t value      = 0;
  bool    hasHit     = false;
  bool    invincible = false;
  bool    critical   = false;
  };

struct Damage final {
  int32_t  val[DamageType::NUM] = {};
  int32_t& operator[](size_t i) { return val[i]; }
  void     operator *= (int32_t v) { for(auto& i:val) i*=v; }
  void     operator /= (int32_t v) { for(auto& i:val) i/=v; }
  };
