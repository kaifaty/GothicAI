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

	};
