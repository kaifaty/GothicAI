#include "testbench_internal.h"

#include <cmath>
#include <cstdio>

#include "game/gamescript.h"
#include "graphics/itemview.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

// The effects of items (graphics/itemview, reports/engine/2026-11-19-item-effects):
//   item put <instance> <dist> <angle> [height]: the item lies in the world by the hero, dropped as a thrown one:
//     dist cm away, angle from the hero's facing (0 in front, as `put`), height cm over the ground (30) it falls from
//   itemfx: the live effects of items and how many of them are nearer the camera than the particles' range
bool itemEffectsCommand(const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="itemfx" && a.size()==1) {
    const auto n = ItemView::census(w);
    say("itemfx alive " + std::to_string(n.alive) + " near " + std::to_string(n.near) +
        (ItemView::enabled() ? "" : " (itemEffects 0)"));
    return true;
    }
  if(c!="item" || a.size()<2 || a[1]!="put")
    return false;
  if(a.size()<5) {
    say("item put: expected item put <instance> <dist> <angle> [height]");
    return true;
    }
  const auto id = w.script().findItemInstance(a[2]);
  if(!id) {
    say("item put: no item " + a[2]);
    return true;
    }
  const float d  = num(a,3,100);
  const float an = (pl.rotation()+num(a,4,0))*float(M_PI)/180.f;
  Vec3 pos = pl.position() + Vec3(std::cos(an),0,std::sin(an))*d;
  // the ground there, by short rays from above (as `put`)
  DynamicWorld::RayLandResult r;
  for(float y=pos.y+1500; y>pos.y-1500 && !r.hasCol; y-=400)
    r = w.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
  if(r.hasCol)
    pos.y = r.v.y;
  pos.y += num(a,5,30);
  auto m = Matrix4x4::mkIdentity();
  m.translate(pos);
  Item* it = w.addItemDyn(*id,m,size_t(-1));
  if(it==nullptr) {
    say("item put: " + a[2] + " was not made");
    return true;
    }
  char buf[240];
  std::snprintf(buf,sizeof(buf),"item put %s at %.0f %.0f %.0f effect %s%s",a[2].c_str(),double(pos.x),double(pos.y),
                double(pos.z),it->handle().effect.empty() ? "-" : it->handle().effect.c_str(),
                r.hasCol ? "" : " (no ground found)");
  say(buf);
  return true;
  }

}
