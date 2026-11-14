#pragma once

#include <cmath>
#include <cstdint>

// The meadow's own ground (reports/engine/2026-11-14-meadow-ground): the `ground` field of a texture's record with
// `grass` - two layers by world position, the sward under the grass and the earth where it is bare - and the
// `verge` field of the ground beside it (a road): the strip along their border takes the meadow's layers, the
// sward comes onto it in tongues. Where the sward lies is one rule for the blades and for the ground's shader:
// grasscover.h; the border itself: grassborder.h. Both work with bindless textures only (six maps); without them
// the ground stays as it is without the fields.
namespace Grass {

struct Ground final {
  bool  on        = false;
  float swardTile = 4.f;   // metres of the world per repeat
  float earthTile = 2.f;
  float far       = 5.3f;  // the sward's colour is taken once more at a tile this much larger, from afar; 0: not
  bool  operator==(const Ground&) const = default;
  };

struct Verge final {
  bool  on     = false;
  float reach  = 90.f;     // cm: the tongues of the sward come this far over the border
  float margin = 50.f;     // cm: the strip of the road where the meadow's layers lie in place of its paint
  bool  operator==(const Verge&) const = default;
  };

constexpr float MaxReach  = 120.f;
constexpr float MaxMargin = 100.f;

// The numbers of `ground`; its six maps are the record's reader's. False on a value of a wrong type or range.
// Value: rapidjson's
template<class Value>
bool parse(const Value& v, Ground& out) {
  if(!v.IsObject())
    return false;
  Ground g;
  g.on = true;
  auto tile = [&](const char* layer, float& dst) {
    const auto l = v.FindMember(layer);
    if(l==v.MemberEnd() || !l->value.IsObject())
      return false;
    const auto it = l->value.FindMember("tile_m");
    if(it==l->value.MemberEnd())
      return true;
    if(!it->value.IsNumber())
      return false;
    const double x = it->value.GetDouble();
    if(!(x>=0.25) || !(x<=64))
      return false;
    dst = float(x);
    return true;
    };
  if(!tile("sward",g.swardTile) || !tile("earth",g.earthTile))
    return false;
  if(const auto it = v.FindMember("far"); it!=v.MemberEnd()) {
    if(!it->value.IsNumber())
      return false;
    const double x = it->value.GetDouble();
    if(!(x==0 || (x>=2 && x<=16)))
      return false;
    g.far = float(x);
    }
  out = g;
  return true;
  }

// The numbers of `verge`; `of` is the record's reader's
template<class Value>
bool parse(const Value& v, Verge& out) {
  if(!v.IsObject())
    return false;
  Verge g;
  g.on = true;
  auto number = [&](const char* field, float& dst, double max) {
    const auto it = v.FindMember(field);
    if(it==v.MemberEnd())
      return true;
    if(!it->value.IsNumber())
      return false;
    const double x = it->value.GetDouble();
    if(!(x>=0) || !(x<=max))
      return false;
    dst = float(x);
    return true;
    };
  if(!number("reach",g.reach,MaxReach) || !number("margin",g.margin,MaxMargin))
    return false;
  out = g;
  return true;
  }

}
