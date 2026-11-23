#pragma once

#include "game/fighttactic.h"

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace FightDefinitions {
inline constexpr std::pair<std::string_view, Def::FightAi FightTactic::*> slots[] = {
  {"FA_ENEMY_PREHIT_",        &FightTactic::enemy_prehit},
  {"FA_ENEMY_STORMPREHIT_",   &FightTactic::enemy_stormprehit},
  {"FA_MY_W_COMBO_",          &FightTactic::my_w_combo},
  {"FA_MY_W_RUNTO_",          &FightTactic::my_w_runto},
  {"FA_MY_W_STRAFE_",         &FightTactic::my_w_strafe},
  {"FA_MY_W_FOCUS_",          &FightTactic::my_w_focus},
  {"FA_MY_W_NOFOCUS_",        &FightTactic::my_w_nofocus},
  {"FA_MY_G_COMBO_",          &FightTactic::my_g_combo},
  {"FA_MY_G_RUNTO_",          &FightTactic::my_g_runto},
  {"FA_MY_G_STRAFE_",         &FightTactic::my_g_strafe},
  {"FA_MY_G_FOCUS_",          &FightTactic::my_g_focus},
  {"FA_MY_FK_FOCUS_",         &FightTactic::my_fk_focus},
  {"FA_MY_G_FK_NOFOCUS_",     &FightTactic::my_g_fk_nofocus},
  {"FA_MY_FK_FOCUS_FAR_",     &FightTactic::my_fk_focus_far},
  {"FA_MY_FK_NOFOCUS_FAR_",   &FightTactic::my_fk_nofocus_far},
  {"FA_MY_FK_FOCUS_MAG_",     &FightTactic::my_fk_focus_mag},
  {"FA_MY_FK_NOFOCUS_MAG_",   &FightTactic::my_fk_nofocus_mag},
  };

std::filesystem::path section(const std::filesystem::path& base, std::string_view program);
std::vector<FightTactic> read(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, const std::vector<FightTactic>& tactics);
}
