#pragma once

#include <zenkit/DaedalusVm.hh>
#include "game/definitions/definitiontypes.h"

class FightAi final {
  public:
    struct FA final {
      Def::FightAi enemy_prehit;      // Enemy attacks me
      Def::FightAi enemy_stormprehit; // Enemy makes a storm attack
      Def::FightAi my_w_combo;        // I'm in the combo window
      Def::FightAi my_w_runto;        // I run towards the opponent
      Def::FightAi my_w_strafe;       // Just take hit
      Def::FightAi my_w_focus;        // I have opponent in focus (can hit)
      Def::FightAi my_w_nofocus;      // I don't have opponent in focus

      // 'G' - range
      Def::FightAi my_g_combo;        // I'm in the combo window (not used in G2)
      Def::FightAi my_g_runto;        // I run towards the opponent (can make a storm attack)
      Def::FightAi my_g_strafe;       // not used in G2
      Def::FightAi my_g_focus;        // I have opponent in focus (can hit)

      // FK Range Foes (far away)
      Def::FightAi my_fk_focus;       // I have opponents in focus

      Def::FightAi my_g_fk_nofocus;   // I am NOT in focus of opponents (also applies to G-distance!)

      // Range + Magic  (used at each removal)
      Def::FightAi my_fk_focus_far;   // Opponents in focus
      Def::FightAi my_fk_nofocus_far; // Opponents NOT in focus

      Def::FightAi my_fk_focus_mag;   // Opponents in focus
      Def::FightAi my_fk_nofocus_mag; // Opponents NOT in focus
      };

    FightAi();

    const FA& operator[](size_t i) const;

  private:
    auto loadAi(zenkit::DaedalusVm &vm, std::string_view name) -> Def::FightAi;
    FA   loadAi(zenkit::DaedalusVm &vm, size_t id);

    std::vector<FA> fAi;
  };
