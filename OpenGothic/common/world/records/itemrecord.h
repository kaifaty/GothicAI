// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Item data extracted from the project copy of ZenKit 1.3.0.

#pragma once

#include "npcrecord.h"

#include <cstdint>
#include <string>

	enum class ItemFlag : std::uint32_t {
		DAGGER = 1U << 13U,
		SWORD = 1U << 14U,
		AXE = 1U << 15U,
		TWO_HANDED_SWORD = 1U << 16U,
		TWO_HANDED_AXE = 1U << 17U,
		BOW = 1U << 19U,
		CROSSBOW = 1U << 20U,
		AMULET = 1U << 22U,
		RING = 1U << 11U,
		BELT = 1U << 24U,
		MISSION = 1U << 12U,

		dagger = DAGGER,
		sword = SWORD,
		axe = AXE,
		two_handed_sword = TWO_HANDED_SWORD,
		two_handed_axe = TWO_HANDED_AXE,
		bow = BOW,
		crossbow = CROSSBOW,
		amulet = AMULET,
		ring = RING,
		belt = BELT,
		mission = MISSION,
	};
	inline ItemFlag operator|(ItemFlag a, ItemFlag b) {
		return static_cast<ItemFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
	}
	inline bool operator&(ItemFlag a, ItemFlag b) {
		return (static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b)) != 0;
	}

	struct ItemRecord {
		using Function = int32_t;
		static constexpr std::uint32_t condition_count = 3;
		static constexpr std::uint32_t state_count = 4;
		static constexpr std::uint32_t text_count = 6;

		int32_t id;
		std::string name;
		std::string name_id;
		int32_t hp;
		int32_t hp_max;
		int32_t main_flag;
		ItemFlag flags;
		int32_t weight;
		int32_t value;
		int32_t damage_type;
		int32_t damage_total;
		int32_t damage[DamageType::NUM];
		int32_t wear;
		int32_t protection[DamageType::NUM];
		int32_t nutrition;
		int32_t cond_atr[condition_count];
		int32_t cond_value[condition_count];
		int32_t change_atr[condition_count];
		int32_t change_value[condition_count];
		Function magic;
		Function on_equip;
		Function on_unequip;
		Function on_state[state_count];
		Function owner;
		int32_t owner_guild;
		int32_t disguise_guild;
		std::string visual;
		std::string visual_change;
		std::string effect;
		int32_t visual_skin;
		std::string scheme_name;
		int32_t material;
		int32_t munition;
		int32_t spell;
		int32_t range;
		int32_t mag_circle;
		std::string description;
		std::string text[text_count];
		int32_t count[text_count];
		int32_t inv_zbias;
		int32_t inv_rot_x;
		int32_t inv_rot_y;
		int32_t inv_rot_z;
		int32_t inv_animate;

	};
