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

		static void fields(auto&& member) {
			member("C_GILVALUES.WATER_DEPTH_KNEE", &GuildRecord::water_depth_knee);
			member("C_GILVALUES.WATER_DEPTH_CHEST", &GuildRecord::water_depth_chest);
			member("C_GILVALUES.JUMPUP_HEIGHT", &GuildRecord::jumpup_height);
			member("C_GILVALUES.SWIM_TIME", &GuildRecord::swim_time);
			member("C_GILVALUES.DIVE_TIME", &GuildRecord::dive_time);
			member("C_GILVALUES.STEP_HEIGHT", &GuildRecord::step_height);
			member("C_GILVALUES.JUMPLOW_HEIGHT", &GuildRecord::jumplow_height);
			member("C_GILVALUES.JUMPMID_HEIGHT", &GuildRecord::jumpmid_height);
			member("C_GILVALUES.SLIDE_ANGLE", &GuildRecord::slide_angle);
			member("C_GILVALUES.SLIDE_ANGLE2", &GuildRecord::slide_angle2);
			member("C_GILVALUES.DISABLE_AUTOROLL", &GuildRecord::disable_autoroll);
			member("C_GILVALUES.SURFACE_ALIGN", &GuildRecord::surface_align);
			member("C_GILVALUES.CLIMB_HEADING_ANGLE", &GuildRecord::climb_heading_angle);
			member("C_GILVALUES.CLIMB_HORIZ_ANGLE", &GuildRecord::climb_horiz_angle);
			member("C_GILVALUES.CLIMB_GROUND_ANGLE", &GuildRecord::climb_ground_angle);
			member("C_GILVALUES.FIGHT_RANGE_BASE", &GuildRecord::fight_range_base);
			member("C_GILVALUES.FIGHT_RANGE_FIST", &GuildRecord::fight_range_fist);
			member("C_GILVALUES.FIGHT_RANGE_1HS", &GuildRecord::fight_range_1hs);
			member("C_GILVALUES.FIGHT_RANGE_1HA", &GuildRecord::fight_range_1ha);
			member("C_GILVALUES.FIGHT_RANGE_2HS", &GuildRecord::fight_range_2hs);
			member("C_GILVALUES.FIGHT_RANGE_2HA", &GuildRecord::fight_range_2ha);
			member("C_GILVALUES.FALLDOWN_HEIGHT", &GuildRecord::falldown_height);
			member("C_GILVALUES.FALLDOWN_DAMAGE", &GuildRecord::falldown_damage);
			member("C_GILVALUES.BLOOD_DISABLED", &GuildRecord::blood_disabled);
			member("C_GILVALUES.BLOOD_MAX_DISTANCE", &GuildRecord::blood_max_distance);
			member("C_GILVALUES.BLOOD_AMOUNT", &GuildRecord::blood_amount);
			member("C_GILVALUES.BLOOD_FLOW", &GuildRecord::blood_flow);
			member("C_GILVALUES.BLOOD_EMITTER", &GuildRecord::blood_emitter);
			member("C_GILVALUES.BLOOD_TEXTURE", &GuildRecord::blood_texture);
			member("C_GILVALUES.TURN_SPEED", &GuildRecord::turn_speed);
			member("C_GILVALUES.FIGHT_RANGE_G", &GuildRecord::fight_range_g);
		}
	};
