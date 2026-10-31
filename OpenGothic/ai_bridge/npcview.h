#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <functional>
#include <string>

class Npc;
class World;

// What an npc perceives right now, for the AI server's two models (protocol v11; v12: the game time of a
// view, the nearest waypoint of whom it sees; v18: how each treats it and the hero, where each is from the
// hero, whom the hero looks at, those close by it notices out of its field of view):
// - `npc_view{req, npcs}`: one view per npc, as the result's value;
// - `npc_views{t, views}`: once a second, the views of the npc's nearest to the player.
// A view: where the npc is (indoors, whose room), what it does and holds; whom it sees and what they look
// like and do; the player, seen or not (it hears him). Sight is the engine's own, as the scripts have it
// (Npc::canSeeNpc: the head's field of view and a ray), within the npc's senses range. Party members see
// hostile living monsters up to 40 m in the same field of view and line of sight.
class NpcView final {
  public:
    using Emit = std::function<void(std::string&&)>;

    explicit NpcView(Emit emit);

    void clear();
    void tick(World& world);
    // a JSON array of the views of the npc's named in `npcs` (script symbols); unknown ones are left out
    std::string views(World& world, const rapidjson::Value& npcs);

  private:
    Emit     emit;
    uint64_t next = 0;
  };

// whether `self` sees `other` now: the engine's own sight, never further than npc_view looks
bool npcSees(const Npc& self, const Npc& other);

// where the player faces, on the ground (x, z): ahead of him and to his right as the screen shows it (a
// mirrored camera swaps the world's right), unit vectors
struct HeroAxes {
  float aheadX = 0, aheadZ = 0;
  float rightX = 0, rightZ = 0;
  };
HeroAxes heroAxes(const Npc& pl);
