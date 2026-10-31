#pragma once

// P13: the engine's own records of the six definition DATs (Camera, Fight, Sfx, Music, ParticleFx,
// VisualFx) and their VM member bindings. The initializers in the P12 program records still fill
// them at the same moment; this replaces only the ZenKit addon types. Field names, types and order
// follow the original classes exactly, including their source spellings.

#include <zenkit/DaedalusScript.hh>

#include <cstdint>
#include <string>

namespace Def {

enum class FightMove : uint32_t {
  NOP           = 0,
  RUN           = 1,
  RUN_BACK      = 2,
  JUMP_BACK     = 3,
  TURN          = 4,
  STRAFE        = 5,
  ATTACK        = 6,
  ATTACK_SIDE   = 7,
  ATTACK_FRONT  = 8,
  ATTACK_TRIPLE = 9,
  ATTACK_WHIRL  = 10,
  ATTACK_MASTER = 11,
  TURN_TO_HIT   = 15,
  PARRY         = 17,
  STAND_UP      = 18,
  WAIT          = 19,
  WAIT_LONGER   = 23,
  WAIT_EXT      = 24,
  };

enum class MusicTransitionEffect : uint32_t {
  UNKNOWN      = 0,
  NONE         = 1,
  GROOVE       = 2,
  FILL         = 3,
  BREAK        = 4,
  INTRO        = 5,
  END          = 6,
  END_AND_INTO = 7,
  };

enum class MusicTransitionType : uint32_t {
  UNKNOWN   = 0,
  IMMEDIATE = 1,
  BEAT      = 2,
  MEASURE   = 3,
  };

// Every bound type declares its fields itself: DaedalusScript::register_member records the declaring
// class and DaedalusVm::init_instance<T> requires typeid(T) to match it. No user-provided
// constructors: make_shared<T>() value-initializes, so scalars start at zero as before.

// CCAMSYS, 23 fields
struct Camera final : zenkit::DaedalusInstance {
  float best_range;
  float min_range;
  float max_range;
  float best_elevation;
  float min_elevation;
  float max_elevation;
  float best_azimuth;
  float min_azimuth;
  float max_azimuth;
  float best_rot_z;
  float min_rot_z;
  float max_rot_z;
  float rot_offset_x;
  float rot_offset_y;
  float rot_offset_z;
  float target_offset_x;
  float target_offset_y;
  float target_offset_z;
  float velo_trans;
  float velo_rot;
  int32_t translate;
  int32_t rotate;
  int32_t collision;

  static void register_(zenkit::DaedalusScript& s);
  };

// C_FIGHTAI stays an opaque VM class (mods often declare MOVE wrongly); this is the copied result.
struct FightAi final {
  static constexpr uint32_t move_count = 6;

  FightMove move[move_count];
  };

// C_SFX, 9 fields
struct SoundEffect final : zenkit::DaedalusInstance {
  std::string file;
  int32_t pitch_off;
  int32_t pitch_var;
  int32_t vol;
  int32_t loop;
  int32_t loop_start_offset;
  int32_t loop_end_offset;
  float reverb_level;
  std::string pfx_name;

  static void register_(zenkit::DaedalusScript& s);
  };

// C_MUSICTHEME, 7 fields
struct MusicTheme final : zenkit::DaedalusInstance {
  std::string file;
  float vol;
  int32_t loop;
  float reverbmix;
  float reverbtime;
  MusicTransitionEffect transtype;
  MusicTransitionType transsubtype;

  static void register_(zenkit::DaedalusScript& s);
  };

// C_PARTICLEFX, 57 fields
struct ParticleEffect final : zenkit::DaedalusInstance {
  float pps_value;
  std::string pps_scale_keys_s;
  int32_t pps_is_looping;
  int32_t pps_is_smooth;
  float pps_fps;
  std::string pps_create_em_s;
  float pps_create_em_delay;
  std::string shp_type_s;
  std::string shp_for_s;
  std::string shp_offset_vec_s;
  std::string shp_distrib_type_s;
  float shp_distrib_walk_speed;
  int32_t shp_is_volume;
  std::string shp_dim_s;
  std::string shp_mesh_s;
  int32_t shp_mesh_render_b;
  std::string shp_scale_keys_s;
  int32_t shp_scale_is_looping;
  int32_t shp_scale_is_smooth;
  float shp_scale_fps;
  std::string dir_mode_s;
  std::string dir_for_s;
  std::string dir_mode_target_for_s;
  std::string dir_mode_target_pos_s;
  float dir_angle_head;
  float dir_angle_head_var;
  float dir_angle_elev;
  float dir_angle_elev_var;
  float vel_avg;
  float vel_var;
  float lsp_part_avg;
  float lsp_part_var;
  std::string fly_gravity_s;
  int32_t fly_colldet_b;
  std::string vis_name_s;
  std::string vis_orientation_s;
  int32_t vis_tex_is_quadpoly;
  float vis_tex_ani_fps;
  int32_t vis_tex_ani_is_looping;
  std::string vis_tex_color_start_s;
  std::string vis_tex_color_end_s;
  std::string vis_size_start_s;
  float vis_size_end_scale;
  std::string vis_alpha_func_s;
  float vis_alpha_start;
  float vis_alpha_end;
  float trl_fade_speed;
  std::string trl_texture_s;
  float trl_width;
  float mrk_fades_peed;
  std::string mrkt_exture_s;
  float mrk_size;
  std::string flock_mode;
  float flock_strength;
  int32_t use_emitters_for;
  std::string time_start_end_s;
  int32_t m_bis_ambient_pfx;

  static void register_(zenkit::DaedalusScript& s);
  };

// CFX_BASE, 46 fields
struct EffectBase final : zenkit::DaedalusInstance {
  static constexpr uint8_t user_string_count = 5;

  std::string vis_name_s;
  std::string vis_size_s;
  float vis_alpha;
  std::string vis_alpha_blend_func_s;
  float vis_tex_ani_fps;
  int32_t vis_tex_ani_is_looping;
  std::string em_trj_mode_s;
  std::string em_trj_origin_node;
  std::string em_trj_target_node;
  float em_trj_target_range;
  float em_trj_target_azi;
  float em_trj_target_elev;
  int32_t em_trj_num_keys;
  int32_t em_trj_num_keys_var;
  float em_trj_angle_elev_var;
  float em_trj_angle_head_var;
  float em_trj_key_dist_var;
  std::string em_trj_loop_mode_s;
  std::string em_trj_ease_func_s;
  float em_trj_ease_vel;
  float em_trj_dyn_update_delay;
  int32_t em_trj_dyn_update_target_only;
  std::string em_fx_create_s;
  std::string em_fx_invest_origin_s;
  std::string em_fx_invest_target_s;
  float em_fx_trigger_delay;
  int32_t em_fx_create_down_trj;
  std::string em_action_coll_dyn_s;
  std::string em_action_coll_stat_s;
  std::string em_fx_coll_stat_s;
  std::string em_fx_coll_dyn_s;
  std::string em_fx_coll_stat_align_s;
  std::string em_fx_coll_dyn_align_s;
  float em_fx_lifespan;
  int32_t em_check_collision;
  int32_t em_adjust_shp_to_origin;
  float em_invest_next_key_duration;
  float em_fly_gravity;
  std::string em_self_rot_vel_s;
  std::string user_string[user_string_count];
  std::string light_preset_name;
  std::string sfx_id;
  int32_t sfx_is_ambient;
  int32_t send_assess_magic;
  float secs_per_damage;
  std::string em_fx_coll_dyn_perc_s;

  static void register_(zenkit::DaedalusScript& s);
  };

// C_PARTICLEFXEMITKEY, 32 fields
struct ParticleEffectEmitKey final : zenkit::DaedalusInstance {
  std::string vis_name_s;
  float vis_size_scale;
  float scale_duration;
  float pfx_pps_value;
  int32_t pfx_pps_is_smooth_chg;
  int32_t pfx_pps_is_looping_chg;
  float pfx_sc_time;
  std::string pfx_fly_gravity_s;
  std::string pfx_shp_dim_s;
  int32_t pfx_shp_is_volume_chg;
  float pfx_shp_scale_fps;
  float pfx_shp_distrib_walks_peed;
  std::string pfx_shp_offset_vec_s;
  std::string pfx_shp_distrib_type_s;
  std::string pfx_dir_mode_s;
  std::string pfx_dir_for_s;
  std::string pfx_dir_mode_target_for_s;
  std::string pfx_dir_mode_target_pos_s;
  float pfx_vel_avg;
  float pfx_lsp_part_avg;
  float pfx_vis_alpha_start;
  std::string light_preset_name;
  float light_range;
  std::string sfx_id;
  int32_t sfx_is_ambient;
  std::string em_create_fx_id;
  float em_fly_gravity;
  std::string em_self_rot_vel_s;
  std::string em_trj_mode_s;
  float em_trj_ease_vel;
  int32_t em_check_collision;
  float em_fx_lifespan;

  static void register_(zenkit::DaedalusScript& s);
  };

}
