// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// NPC data extracted from the project copy of ZenKit 1.3.0.

#pragma once

#include <cstdint>
#include <string>

	namespace DamageType {
		static constexpr std::uint32_t BARRIER = 0U;
		static constexpr std::uint32_t BLUNT = 1U;
		static constexpr std::uint32_t EDGE = 2U;
		static constexpr std::uint32_t FIRE = 3U;
		static constexpr std::uint32_t FLY = 4U;
		static constexpr std::uint32_t MAGIC = 5U;
		static constexpr std::uint32_t POINT = 6U;
		static constexpr std::uint32_t FALL = 7U;

		static constexpr std::uint32_t NUM = 8U;

		static constexpr std::uint32_t barrier = BARRIER;
		static constexpr std::uint32_t blunt = BLUNT;
		static constexpr std::uint32_t edge = EDGE;
		static constexpr std::uint32_t fire = FIRE;
		static constexpr std::uint32_t fly = FLY;
		static constexpr std::uint32_t magic = MAGIC;
		static constexpr std::uint32_t point = POINT;
		static constexpr std::uint32_t fall = FALL;
		static constexpr std::uint32_t count = NUM;
	} // namespace DamageType

	namespace NpcAttribute {
		static constexpr std::uint32_t HITPOINTS = 0U;
		static constexpr std::uint32_t HITPOINTS_MAX = 1U;
		static constexpr std::uint32_t MANA = 2U;
		static constexpr std::uint32_t MANA_MAX = 3U;
		static constexpr std::uint32_t STRENGTH = 4U;
		static constexpr std::uint32_t DEXTERITY = 5U;
		static constexpr std::uint32_t REGENERATE_HP = 6U;
		static constexpr std::uint32_t REGENERATE_MANA = 7U;

		static constexpr std::uint32_t NUM = 8U;

		static constexpr std::uint32_t hitpoints = HITPOINTS;
		static constexpr std::uint32_t hitpoints_max = HITPOINTS_MAX;
		static constexpr std::uint32_t mana = MANA;
		static constexpr std::uint32_t mana_max = MANA_MAX;
		static constexpr std::uint32_t strength = STRENGTH;
		static constexpr std::uint32_t dexterity = DEXTERITY;
		static constexpr std::uint32_t regenerate_hp = REGENERATE_HP;
		static constexpr std::uint32_t
		    regenerate_mana = REGENERATE_MANA;
		static constexpr std::uint32_t count = NUM;
	} // namespace NpcAttribute

	enum class NpcType : std::uint32_t {
		G1_AMBIENT = 0U,
		G1_MAIN = 1U,
		G1_GUARD = 2U,
		G1_FRIEND = 3U,
		G1_MINE_AMBIENT = 4U,
		G1_MINE_GUARD = 5U,
		G1_OW_AMBIENT = 6U,
		G1_OW_GUARD = 7U,
		G1_ROGUE = 8U,

		G2_AMBIENT = 0U,
		G2_MAIN = 1U,
		G2_FRIEND = 2U,
		G2_OC_AMBIENT = 3U,
		G2_OC_MAIN = 4U,
		G2_BL_AMBIENT = 5U,
		G2_TAL_AMBIENT = 6U,
		G2_BL_MAIN = 7U,

		AMBIENT = G2_AMBIENT,
		MAIN = G2_MAIN,

		g1_ambient = G1_AMBIENT,
		g1_main = G1_MAIN,
		g1_guard = G1_GUARD,
		g1_friend = G1_FRIEND,
		g1_mine_ambient = G1_MINE_AMBIENT,
		g1_mine_guard = G1_MINE_GUARD,
		g1_ow_ambient = G1_OW_AMBIENT,
		g1_ow_guard = G1_OW_GUARD,
		g1_rogue = G1_ROGUE,

		g2_ambient = G2_AMBIENT,
		g2_main = G2_MAIN,
		g2_friend = G2_FRIEND,
		g2_oc_ambient = G2_OC_AMBIENT,
		g2_oc_main = G2_OC_MAIN,
		g2_bl_ambient = G2_BL_AMBIENT,
		g2_tal_ambient = G2_TAL_AMBIENT,
		g2_bl_main = G2_BL_MAIN,

		ambient = AMBIENT,
		main = MAIN,

		friend_ = G2_FRIEND,
		oc_ambient = G2_OC_AMBIENT,
		oc_main = G2_OC_MAIN,
		bl_ambient = G2_BL_AMBIENT,
		tal_ambient = G2_TAL_AMBIENT,
		bl_main = G2_BL_MAIN,
	};

	static_assert(NpcType::G1_AMBIENT == NpcType::G2_AMBIENT);
	static_assert(NpcType::G1_MAIN == NpcType::G2_MAIN);

	enum class NpcFlag : std::uint32_t {
		NONE = 0U,
		FRIENDS = 1U << 0U,
		IMMORTAL = 1U << 1U,
		GHOST = 1U << 2U,
		PROTECTED = 1U << 3U,

		none = NONE,
		friends = FRIENDS,
		immortal = IMMORTAL,
		ghost = GHOST,
		protected_ = PROTECTED,
	};
	inline NpcFlag operator|(NpcFlag a, NpcFlag b) {
		return static_cast<NpcFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
	}
	inline bool operator&(NpcFlag a, NpcFlag b) {
		return (static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b)) != 0;
	}

	struct NpcRecord {
		using Function = int32_t;
		static constexpr std::uint32_t hitchance_count =
		    5; // -> TODO: one of "unknown", "one-handed", "two-handed", "bow", "crossbow"
		static constexpr std::uint32_t name_count = 5;
		static constexpr std::uint32_t mission_count = 5;
		static constexpr std::uint32_t aivar_count = 100;

		int32_t id;
		std::string name[name_count];
		std::string slot;
		std::string effect;
		NpcType type;
		NpcFlag flags;
		int32_t attribute[NpcAttribute::NUM];
		int32_t hitchance[hitchance_count];
		int32_t protection[DamageType::NUM];
		int32_t damage[DamageType::NUM];
		int32_t damage_type;
		int32_t guild;
		int32_t level;
		Function mission[mission_count];
		int32_t fight_tactic;
		int32_t weapon;
		int32_t voice;
		int32_t voice_pitch;
		int32_t body_mass;
		Function daily_routine;
		Function start_aistate;
		std::string spawnpoint;
		int32_t spawn_delay;
		int32_t senses;
		int32_t senses_range;
		int32_t aivar[aivar_count];
		std::string wp;
		int32_t exp;
		int32_t exp_next;
		int32_t lp;
		int32_t bodystate_interruptable_override;
		int32_t no_focus;

	};
