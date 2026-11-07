#pragma once

#include "eval.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace daedalus {
  struct INpc;
  struct IItem;
  }

namespace zenkit {
  struct VNpc;
  class  WayNet;
  }

namespace storygraph {

// The world of an original Gothic II save (ZenGin format: SAVEDAT.SAV and <WORLD>.SAV) as dialog conditions
// see it: story variables, the dialogs the hero knows, npcs with attributes, inventories, AI states and
// positions. Engine functions answer from it, mirroring OpenGothic; the ones it does not model return
// 0, "" or null and are counted, so a check run shows how much of the world it actually exercised.
class SaveWorld final : public Externals {
  public:
    explicit SaveWorld(daedalus::DaedalusVm& vm);

    // false and `error` when the script state can't be read; worlds that can't be read are skipped
    // (`worldErrors`), their npcs are then missing
    bool  load(const std::filesystem::path& dir, std::string& error);
    Value call(const daedalus::DaedalusSymbol& fn, const std::vector<Value>& args) override;

    struct Npc final {
      std::shared_ptr<zenkit::VNpc>        save;
      std::shared_ptr<daedalus::INpc>        script;
      std::string                          world;
      float                                x = 0, y = 0, z = 0;  // where it stands, or its routine waypoint
      bool                                 placed = false;      // position known
      std::unordered_map<uint32_t,int32_t> items;               // item instance -> amount
      };

    // the npc of a script instance, nullptr if it is not in the save
    const Npc*                    npc(uint32_t instanceSymbol) const;
    const Npc*                    npc(const daedalus::DaedalusInstance* script) const;
    size_t                        npcCount() const { return npcs.size(); }
    // the npc the hero is talking to: its AI state is ZS_TALK, like when the engine builds the dialog menu
    void                          talkingTo(const daedalus::DaedalusInstance* script) { partner = script; }

    std::string                   title, world;
    int32_t                       day = 0, hour = 0, minute = 0, kapitel = 0;
    std::shared_ptr<daedalus::INpc> hero;
    bool                          heroStandIn = false;       // no world was read: the dialogs the hero knows still count
    std::vector<std::string>      worlds, worldErrors;
    size_t                        npcsSkipped = 0;           // instances not in the DAT
    size_t                        symbolsSet = 0, symbolsMissing = 0;

    // engine functions called since the last reset: answered from the save, or with a default
    std::map<std::string,size_t>  answered, defaulted;
    void                          resetCounts() { answered.clear(); defaulted.clear(); }
    // when set, every call is reported: "NPC_HASITEMS(other, ITPL_BEET) = 3"
    std::function<void(const std::string&)> trace;

  private:
    using Handler = std::function<Value(const std::vector<Value>&)>;
    void         addHandlers();
    void         addNpc(const std::shared_ptr<zenkit::VNpc>& v, const std::string& world, const zenkit::WayNet* wn,
                        bool placed);
    const Npc*   arg(const std::vector<Value>& a, size_t i) const;
    int32_t      state(const Npc& n) const;
    int32_t      constant(const char* name, int32_t def) const;
    std::shared_ptr<daedalus::IItem> item(uint32_t symbol);
    std::shared_ptr<daedalus::IItem> equipped(const Npc* n, uint32_t category);

    daedalus::DaedalusVm&                                vm;
    std::vector<Npc>                                   npcs;
    std::unordered_map<uint32_t,size_t>                bySymbol;
    std::unordered_map<const daedalus::DaedalusInstance*,size_t> byScript;
    std::map<std::string,std::map<std::string,std::array<float,3>>> waypoints;  // world -> name -> position
    std::set<uint32_t>                                 told;     // dialogs the hero knows
    uint8_t                                            guildAttitudes[42][42] = {};
    std::unordered_map<uint32_t,std::shared_ptr<daedalus::IItem>> items;
    std::unordered_map<std::string,Handler>            handlers;
    const daedalus::DaedalusInstance*                    partner = nullptr;
    int32_t                                            zsTalk = -1;
  };

}
