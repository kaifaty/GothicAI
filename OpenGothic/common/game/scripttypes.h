#pragma once

#include "rpgvocabulary.h"
#include "world/records/recordinstance.h"

#include <cstddef>
#include <cstdint>
#include <string>

class ScriptFn final {
  public:
    ScriptFn()=default;
    ScriptFn(size_t v):ptr(v){}

    size_t ptr=size_t(-1);

    bool isValid() const { return ptr!=size_t(-1); }

  friend bool operator == (const ScriptFn& a,const ScriptFn& b) {
    return a.ptr==b.ptr;
    }
  };

struct DlgChoice final {
  std::string      title;
  int32_t          sort     = 0;
  uint32_t         scriptFn = 0;
  bool             isTrade  = false;
  daedalus::IInfo*   handle   = nullptr;
  std::string      external;  // a dialog made at runtime (ai_bridge): its id; handle and scriptFn are empty
  };

struct PerDist {
  PerDist() {
    for(auto& i:range)
      i = -1;
    }
  int32_t range[PERC_Count] = {};
  int     at(PercType perc, int r) const {
    if(perc>=PERC_Count)
      return r;
    auto rr = range[perc];
    if(rr>0)
      return rr;
    return r;
    }
  };

enum class PhysicalSpell : uint8_t { None, WindFist, Whirlwind, Fireball, IceCube };
