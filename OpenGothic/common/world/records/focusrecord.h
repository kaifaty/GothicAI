// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Focus data extracted from the project copy of ZenKit 1.3.0.

#pragma once

#include <cstdint>

	struct FocusRecord {
		float npc_longrange;
		float npc_range1;
		float npc_range2;
		float npc_azi;
		float npc_elevdo;
		float npc_elevup;
		int32_t npc_prio;
		float item_range1;
		float item_range2;
		float item_azi;
		float item_elevdo;
		float item_elevup;
		int32_t item_prio;
		float mob_range1;
		float mob_range2;
		float mob_azi;
		float mob_elevdo;
		float mob_elevup;
		int32_t mob_prio;

		static void fields(auto&& member) {
			member("C_FOCUS.NPC_LONGRANGE", &FocusRecord::npc_longrange);
			member("C_FOCUS.NPC_RANGE1", &FocusRecord::npc_range1);
			member("C_FOCUS.NPC_RANGE2", &FocusRecord::npc_range2);
			member("C_FOCUS.NPC_AZI", &FocusRecord::npc_azi);
			member("C_FOCUS.NPC_ELEVDO", &FocusRecord::npc_elevdo);
			member("C_FOCUS.NPC_ELEVUP", &FocusRecord::npc_elevup);
			member("C_FOCUS.NPC_PRIO", &FocusRecord::npc_prio);
			member("C_FOCUS.ITEM_RANGE1", &FocusRecord::item_range1);
			member("C_FOCUS.ITEM_RANGE2", &FocusRecord::item_range2);
			member("C_FOCUS.ITEM_AZI", &FocusRecord::item_azi);
			member("C_FOCUS.ITEM_ELEVDO", &FocusRecord::item_elevdo);
			member("C_FOCUS.ITEM_ELEVUP", &FocusRecord::item_elevup);
			member("C_FOCUS.ITEM_PRIO", &FocusRecord::item_prio);
			member("C_FOCUS.MOB_RANGE1", &FocusRecord::mob_range1);
			member("C_FOCUS.MOB_RANGE2", &FocusRecord::mob_range2);
			member("C_FOCUS.MOB_AZI", &FocusRecord::mob_azi);
			member("C_FOCUS.MOB_ELEVDO", &FocusRecord::mob_elevdo);
			member("C_FOCUS.MOB_ELEVUP", &FocusRecord::mob_elevup);
			member("C_FOCUS.MOB_PRIO", &FocusRecord::mob_prio);
		}
	};
