#pragma once

#include <zenkit/Misc.hh>
#include <cstdint>
#include <string>

namespace asset {

// Numeric values are part of the existing Gothic material records in the base.
enum class AlphaFunction { DEFAULT=0, NONE=1, BLEND=2, ADD=3, SUBTRACT=4, MULTIPLY=5, MULTIPLY_ALT=6 };
enum class MaterialGroup : uint8_t { UNDEFINED=0, METAL=1, STONE=2, WOOD=3, EARTH=4, WATER=5, SNOW=6, NONE=0xff };
enum class WaveSpeed : uint8_t { NONE=0, SLOW=1, NORMAL=2, FAST=3 };
enum class WaveMode : uint8_t { NONE=0, GROUND_AMBIENT=1, GROUND=2, WALL_AMBIENT=3, WALL=4, ENVIRONMENT=5, WIND_AMBIENT=6, WIND=7 };
enum class AnimationMapping { NONE=0, LINEAR=1 };

struct Material {
  std::string name;
  MaterialGroup group = MaterialGroup::UNDEFINED;
  zenkit::Color color {0,0,0,0};
  float smooth_angle = 0;
  std::string texture;
  zenkit::Vec2 texture_scale {};
  float texture_anim_fps = 0;
  AnimationMapping texture_anim_map_mode = AnimationMapping::NONE;
  zenkit::Vec2 texture_anim_map_dir {};
  bool disable_collision = false;
  bool disable_lightmap = false;
  bool dont_collapse = false;
  std::string detail_object;
  float detail_object_scale = 0;
  bool force_occluder = false;
  bool environment_mapping = false;
  float environment_mapping_strength = 0;
  WaveMode wave_mode = WaveMode::NONE;
  WaveSpeed wave_speed = WaveSpeed::NONE;
  float wave_max_amplitude = 0;
  float wave_grid_size = 0;
  bool ignore_sun = false;
  AlphaFunction alpha_func = AlphaFunction::NONE;
  zenkit::Vec2 default_mapping {};
  };

}
