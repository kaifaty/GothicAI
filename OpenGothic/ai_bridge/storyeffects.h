#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <string>
#include <string_view>

class Npc;

namespace zenkit {
  class DaedalusVm;
  }

// What the director makes happen, through the scripts' own functions (docs/story-graph.md, stages 3–4):
// in a dialog of its own, or as the consequence of a lie found out.
struct StoryEffect final {
  std::string kind;     // give, xp, set, journal, end, attack, refuse_talk, routine, create, teach, calm, party,
                        // knows, crime_memory, chest_take
  std::string item;     // give, create, chest_take: item instance
  std::string chest;    // chest_take (v48): source chest's NpcChests id
  std::string teach;    // teach: attribute, fight, circle, rune, thief, alchemy, smith, trophy (v17)
  std::string arg;      // teach: the scripts' constant (ATR_STRENGTH, NPC_TALENT_1H, SPL_*, POTION_*...)
  std::string var;      // set: global int variable
  std::string target;   // routine: the npc whose routine changes (instance symbol), empty: this npc;
                        // give with to=npc (v39): the npc that gets it
  std::string routine;  // routine: its name in the scripts (Rtn_<name>_<id>), e.g. START
  std::string topic;    // journal: topic, game codepage
  std::string text;     // journal: entry, game codepage
  std::string status;   // journal: LOG_RUNNING (default), LOG_SUCCESS, LOG_FAILED, LOG_OBSOLETE
  std::string reason;   // attack: AR_* constant (default AR_NONE: until the hero is down, not to death)
  std::string attitude; // calm: ATT_* constant, how it treats the hero from now on (default ATT_NEUTRAL; v20)
  std::string info;     // knows: a dialog (C_INFO) the hero has heard from now on, as when it is played (v31)
  int32_t     count  = 1;  // give, create, chest_take: count; xp: amount; set: value; refuse_talk: seconds;
                           // teach: points (attribute), percent (fight), circle (circle); party: 1 on, 0 off
  int32_t     day    = 0;  // crime_memory: the remembered deed's day
  bool        toHero = true;  // give: the npc gives to the hero, else the hero to the npc
  bool        toNpc  = false; // give (v39): the npc gives to another npc (`target`), far from the hero

  // `{"do": "give", "item": ..., ...}`; an error if it is malformed or, with a VM, names something
  // the scripts don't have
  static std::string read(const rapidjson::Value& v, zenkit::DaedalusVm* vm, StoryEffect& e);
  // with the npc as self and the hero as other; failures are logged with `who`
  void run(zenkit::DaedalusVm& vm, Npc& npc, Npc& player, std::string_view who) const;
  // teach: the learning points it takes from the hero, as the scripts' teacher function counts them;
  // -1 and `error` if the scripts can't say
  int32_t learnCost(zenkit::DaedalusVm& vm, Npc& player, std::string& error) const;
  };
