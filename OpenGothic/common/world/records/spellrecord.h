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

		static void fields(auto&& member) {
			member("C_SPELL.TIME_PER_MANA", &SpellRecord::time_per_mana);
			member("C_SPELL.DAMAGE_PER_LEVEL", &SpellRecord::damage_per_level);
			member("C_SPELL.DAMAGETYPE", &SpellRecord::damage_type);
			member("C_SPELL.SPELLTYPE", &SpellRecord::spell_type);
			member("C_SPELL.CANTURNDURINGINVEST", &SpellRecord::can_turn_during_invest);
			member("C_SPELL.CANCHANGETARGETDURINGINVEST", &SpellRecord::can_change_target_during_invest);
			member("C_SPELL.ISMULTIEFFECT", &SpellRecord::is_multi_effect);
			member("C_SPELL.TARGETCOLLECTALGO", &SpellRecord::target_collect_algo);
			member("C_SPELL.TARGETCOLLECTTYPE", &SpellRecord::target_collect_type);
			member("C_SPELL.TARGETCOLLECTRANGE", &SpellRecord::target_collect_range);
			member("C_SPELL.TARGETCOLLECTAZI", &SpellRecord::target_collect_azi);
			member("C_SPELL.TARGETCOLLECTELEV", &SpellRecord::target_collect_elev);
		}
	};
