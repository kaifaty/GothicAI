// Copyright © 2021-2024 GothicKit Contributors.
// SPDX-License-Identifier: MIT
// Guild data extracted from the project copy of ZenKit 1.3.0.

#pragma once

#include <cstdint>
#include <string>

	struct GuildRecord {
		static constexpr std::uint32_t count = 66;

		int32_t water_depth_knee[count];
		int32_t water_depth_chest[count];
		int32_t jumpup_height[count];
		int32_t swim_time[count];
		int32_t dive_time[count];
		int32_t step_height[count];
		int32_t jumplow_height[count];
		int32_t jumpmid_height[count];
		int32_t slide_angle[count];
		int32_t slide_angle2[count];
		int32_t disable_autoroll[count];
		int32_t surface_align[count];
		int32_t climb_heading_angle[count];
		int32_t climb_horiz_angle[count];
		int32_t climb_ground_angle[count];
		int32_t fight_range_base[count];
		int32_t fight_range_fist[count];
		int32_t fight_range_g[count];
		int32_t fight_range_1hs[count];
		int32_t fight_range_1ha[count];
		int32_t fight_range_2hs[count];
		int32_t fight_range_2ha[count];
		int32_t falldown_height[count];
		int32_t falldown_damage[count];
		int32_t blood_disabled[count];
		int32_t blood_max_distance[count];
		int32_t blood_amount[count];
		int32_t blood_flow[count];
		std::string blood_emitter[count];
		std::string blood_texture[count];
		int32_t turn_speed[count];
	};
