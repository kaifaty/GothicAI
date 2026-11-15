// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Spell data extracted from the project copy of ZenKit 1.3.0.

#pragma once

#include <cstdint>

	struct SpellRecord {
		float time_per_mana;
		int32_t damage_per_level;
		int32_t damage_type;
		int32_t spell_type;
		int32_t can_turn_during_invest;
		int32_t can_change_target_during_invest;
		int32_t is_multi_effect;
		int32_t target_collect_algo;
		int32_t target_collect_type;
		int32_t target_collect_range;
		int32_t target_collect_azi;
		int32_t target_collect_elev;
	};
