#pragma once

#include "game/gametime.h"

#include <string>
#include <string_view>
#include <vector>

class Interactive;
class Npc;
class World;

// Places of world objects for routine activities (step E1, world/activityevents.h). A place is
// free, reserved (its NPC walks from the routine point to the object) or occupied (an NPC is
// attached: Interactive's own state, saved with the object). This table owns the reservations and
// the NPCs that wait for an object of a scheme. A reservation leaves only through release() and
// drop(); a freed place wakes the waiting NPCs near it. Nothing here is saved: after a load the
// occupied places are the restored attachments and there are no reservations yet.
class MobReservations final {
  public:
    explicit MobReservations(World& world);

    // `npc` may take `mob`: nobody is attached and no other NPC holds its reservation.
    bool         isFree(const Interactive& mob, const Npc& npc) const;
    Interactive* reserved(const Npc& npc) const;
    // Replaces the NPC's previous reservation; false when the place is not free for it.
    bool         reserve(Npc& npc, Interactive& mob);
    // The one way an owner gives its reservation up: the end of its activity, a reset queue, removal.
    void         release(const Npc& npc);

    // Npc::setInteraction and Npc::quitInteraction: every attach and detach of every NPC.
    void         attached(Npc& npc, Interactive& mob);
    void         detached(Npc& npc, Interactive& mob);

    void         wait(Npc& npc, std::string_view scheme);
    void         unwait(const Npc& npc);

    // Every world tick: expired reservations and reservations without a living owner.
    void         tick();
    // Counts and drops reservations without a living owner. `final` (the end of a journal) only
    // counts, and also checks that every owner is still in the world.
    void         audit(bool final);

  private:
    struct Reservation {
      Npc*         owner = nullptr;
      Interactive* mob   = nullptr;
      gtime        until;
      };
    struct Waiter {
      Npc*        npc = nullptr;
      std::string scheme;
      };
    void         drop(size_t i);
    void         freed(const Interactive& mob);

    World&                   world;
    std::vector<Reservation> reservations;
    std::vector<Waiter>      waiters;
  };
