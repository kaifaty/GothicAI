#pragma once

// P13 definition values, copied from script instances by the Gothic layer.
// Field lists keep record layout, VM bindings and copying in sync. No constructors:
// value-initialization leaves scalars at zero, as in the original script classes.

#include <cstdint>
#include <string>

enum class SpellFxKey:uint8_t {
  Open,
  Init,
  Cast,
  Invest,
  Collide,
  Count
  };

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

#define DEF_DECLARE_FIELD(type, name, extent, symbol) type name extent;

#define DEF_CAMERA_FIELDS(F) \
  F(float, best_range, , "BESTRANGE") \
  F(float, min_range, , "MINRANGE") \
  F(float, max_range, , "MAXRANGE") \
  F(float, best_elevation, , "BESTELEVATION") \
  F(float, min_elevation, , "MINELEVATION") \
  F(float, max_elevation, , "MAXELEVATION") \
  F(float, best_azimuth, , "BESTAZIMUTH") \
  F(float, min_azimuth, , "MINAZIMUTH") \
  F(float, max_azimuth, , "MAXAZIMUTH") \
  F(float, best_rot_z, , "BESTROTZ") \
  F(float, min_rot_z, , "MINROTZ") \
  F(float, max_rot_z, , "MAXROTZ") \
  F(float, rot_offset_x, , "ROTOFFSETX") \
  F(float, rot_offset_y, , "ROTOFFSETY") \
  F(float, rot_offset_z, , "ROTOFFSETZ") \
  F(float, target_offset_x, , "TARGETOFFSETX") \
  F(float, target_offset_y, , "TARGETOFFSETY") \
  F(float, target_offset_z, , "TARGETOFFSETZ") \
  F(float, velo_trans, , "VELOTRANS") \
  F(float, velo_rot, , "VELOROT") \
  F(int32_t, translate, , "TRANSLATE") \
  F(int32_t, rotate, , "ROTATE") \
  F(int32_t, collision, , "COLLISION")

// CCAMSYS, 23 fields
struct Camera final {
  DEF_CAMERA_FIELDS(DEF_DECLARE_FIELD)
  };

// C_FIGHTAI stays an opaque VM class (mods often declare MOVE wrongly); this is the copied result.
struct FightAi final {
  static constexpr uint32_t move_count = 6;

  FightMove move[move_count];
  };

#define DEF_SOUND_EFFECT_FIELDS(F) \
  F(std::string, file, , "FILE") \
  F(int32_t, pitch_off, , "PITCHOFF") \
  F(int32_t, pitch_var, , "PITCHVAR") \
  F(int32_t, vol, , "VOL") \
  F(int32_t, loop, , "LOOP") \
  F(int32_t, loop_start_offset, , "LOOPSTARTOFFSET") \
  F(int32_t, loop_end_offset, , "LOOPENDOFFSET") \
  F(float, reverb_level, , "REVERBLEVEL") \
  F(std::string, pfx_name, , "PFXNAME")

// C_SFX, 9 fields
struct SoundEffect final {
  DEF_SOUND_EFFECT_FIELDS(DEF_DECLARE_FIELD)
  };

#define DEF_MUSIC_THEME_FIELDS(F) \
  F(std::string, file, , "FILE") \
  F(float, vol, , "VOL") \
  F(int32_t, loop, , "LOOP") \
  F(float, reverbmix, , "REVERBMIX") \
  F(float, reverbtime, , "REVERBTIME") \
  F(Def::MusicTransitionEffect, transtype, , "TRANSTYPE") \
  F(Def::MusicTransitionType, transsubtype, , "TRANSSUBTYPE")

// C_MUSICTHEME, 7 fields
struct MusicTheme final {
  DEF_MUSIC_THEME_FIELDS(DEF_DECLARE_FIELD)
  };

#define DEF_PARTICLE_EFFECT_COMMON_FIELDS(F) \
  F(float, pps_value, , "PPSVALUE") \
  F(std::string, pps_scale_keys_s, , "PPSSCALEKEYS_S") \
  F(int32_t, pps_is_looping, , "PPSISLOOPING") \
  F(int32_t, pps_is_smooth, , "PPSISSMOOTH") \
  F(float, pps_fps, , "PPSFPS") \
  F(std::string, pps_create_em_s, , "PPSCREATEEM_S") \
  F(float, pps_create_em_delay, , "PPSCREATEEMDELAY") \
  F(std::string, shp_type_s, , "SHPTYPE_S") \
  F(std::string, shp_for_s, , "SHPFOR_S") \
  F(std::string, shp_offset_vec_s, , "SHPOFFSETVEC_S") \
  F(std::string, shp_distrib_type_s, , "SHPDISTRIBTYPE_S") \
  F(float, shp_distrib_walk_speed, , "SHPDISTRIBWALKSPEED") \
  F(int32_t, shp_is_volume, , "SHPISVOLUME") \
  F(std::string, shp_dim_s, , "SHPDIM_S") \
  F(std::string, shp_mesh_s, , "SHPMESH_S") \
  F(int32_t, shp_mesh_render_b, , "SHPMESHRENDER_B") \
  F(std::string, shp_scale_keys_s, , "SHPSCALEKEYS_S") \
  F(int32_t, shp_scale_is_looping, , "SHPSCALEISLOOPING") \
  F(int32_t, shp_scale_is_smooth, , "SHPSCALEISSMOOTH") \
  F(float, shp_scale_fps, , "SHPSCALEFPS") \
  F(std::string, dir_mode_s, , "DIRMODE_S") \
  F(std::string, dir_for_s, , "DIRFOR_S") \
  F(std::string, dir_mode_target_for_s, , "DIRMODETARGETFOR_S") \
  F(std::string, dir_mode_target_pos_s, , "DIRMODETARGETPOS_S") \
  F(float, dir_angle_head, , "DIRANGLEHEAD") \
  F(float, dir_angle_head_var, , "DIRANGLEHEADVAR") \
  F(float, dir_angle_elev, , "DIRANGLEELEV") \
  F(float, dir_angle_elev_var, , "DIRANGLEELEVVAR") \
  F(float, vel_avg, , "VELAVG") \
  F(float, vel_var, , "VELVAR") \
  F(float, lsp_part_avg, , "LSPPARTAVG") \
  F(float, lsp_part_var, , "LSPPARTVAR") \
  F(std::string, fly_gravity_s, , "FLYGRAVITY_S") \
  F(int32_t, fly_colldet_b, , "FLYCOLLDET_B") \
  F(std::string, vis_name_s, , "VISNAME_S") \
  F(std::string, vis_orientation_s, , "VISORIENTATION_S") \
  F(int32_t, vis_tex_is_quadpoly, , "VISTEXISQUADPOLY") \
  F(float, vis_tex_ani_fps, , "VISTEXANIFPS") \
  F(int32_t, vis_tex_ani_is_looping, , "VISTEXANIISLOOPING") \
  F(std::string, vis_tex_color_start_s, , "VISTEXCOLORSTART_S") \
  F(std::string, vis_tex_color_end_s, , "VISTEXCOLOREND_S") \
  F(std::string, vis_size_start_s, , "VISSIZESTART_S") \
  F(float, vis_size_end_scale, , "VISSIZEENDSCALE") \
  F(std::string, vis_alpha_func_s, , "VISALPHAFUNC_S") \
  F(float, vis_alpha_start, , "VISALPHASTART") \
  F(float, vis_alpha_end, , "VISALPHAEND") \
  F(float, trl_fade_speed, , "TRLFADESPEED") \
  F(std::string, trl_texture_s, , "TRLTEXTURE_S") \
  F(float, trl_width, , "TRLWIDTH") \
  F(float, mrk_fades_peed, , "MRKFADESPEED") \
  F(std::string, mrkt_exture_s, , "MRKTEXTURE_S") \
  F(float, mrk_size, , "MRKSIZE")

#define DEF_PARTICLE_EFFECT_G2_FIELDS(F) \
  F(std::string, flock_mode, , "FLOCKMODE") \
  F(float, flock_strength, , "FLOCKSTRENGTH") \
  F(int32_t, use_emitters_for, , "USEEMITTERSFOR") \
  F(std::string, time_start_end_s, , "TIMESTARTEND_S") \
  F(int32_t, m_bis_ambient_pfx, , "M_BISAMBIENTPFX")

#define DEF_PARTICLE_EFFECT_FIELDS(F) \
  DEF_PARTICLE_EFFECT_COMMON_FIELDS(F) \
  DEF_PARTICLE_EFFECT_G2_FIELDS(F)

// C_PARTICLEFX, 57 fields
struct ParticleEffect final {
  DEF_PARTICLE_EFFECT_FIELDS(DEF_DECLARE_FIELD)
  };

#define DEF_EFFECT_BASE_COMMON_FIELDS(F) \
  F(std::string, vis_name_s, , "VISNAME_S") \
  F(std::string, vis_size_s, , "VISSIZE_S") \
  F(float, vis_alpha, , "VISALPHA") \
  F(std::string, vis_alpha_blend_func_s, , "VISALPHABLENDFUNC_S") \
  F(float, vis_tex_ani_fps, , "VISTEXANIFPS") \
  F(int32_t, vis_tex_ani_is_looping, , "VISTEXANIISLOOPING") \
  F(std::string, em_trj_mode_s, , "EMTRJMODE_S") \
  F(std::string, em_trj_origin_node, , "EMTRJORIGINNODE") \
  F(std::string, em_trj_target_node, , "EMTRJTARGETNODE") \
  F(float, em_trj_target_range, , "EMTRJTARGETRANGE") \
  F(float, em_trj_target_azi, , "EMTRJTARGETAZI") \
  F(float, em_trj_target_elev, , "EMTRJTARGETELEV") \
  F(int32_t, em_trj_num_keys, , "EMTRJNUMKEYS") \
  F(int32_t, em_trj_num_keys_var, , "EMTRJNUMKEYSVAR") \
  F(float, em_trj_angle_elev_var, , "EMTRJANGLEELEVVAR") \
  F(float, em_trj_angle_head_var, , "EMTRJANGLEHEADVAR") \
  F(float, em_trj_key_dist_var, , "EMTRJKEYDISTVAR") \
  F(std::string, em_trj_loop_mode_s, , "EMTRJLOOPMODE_S") \
  F(std::string, em_trj_ease_func_s, , "EMTRJEASEFUNC_S") \
  F(float, em_trj_ease_vel, , "EMTRJEASEVEL") \
  F(float, em_trj_dyn_update_delay, , "EMTRJDYNUPDATEDELAY") \
  F(int32_t, em_trj_dyn_update_target_only, , "EMTRJDYNUPDATETARGETONLY") \
  F(std::string, em_fx_create_s, , "EMFXCREATE_S") \
  F(std::string, em_fx_invest_origin_s, , "EMFXINVESTORIGIN_S") \
  F(std::string, em_fx_invest_target_s, , "EMFXINVESTTARGET_S") \
  F(float, em_fx_trigger_delay, , "EMFXTRIGGERDELAY") \
  F(int32_t, em_fx_create_down_trj, , "EMFXCREATEDOWNTRJ") \
  F(std::string, em_action_coll_dyn_s, , "EMACTIONCOLLDYN_S") \
  F(std::string, em_action_coll_stat_s, , "EMACTIONCOLLSTAT_S") \
  F(std::string, em_fx_coll_stat_s, , "EMFXCOLLSTAT_S") \
  F(std::string, em_fx_coll_dyn_s, , "EMFXCOLLDYN_S") \
  F(std::string, em_fx_coll_stat_align_s, , "EMFXCOLLSTATALIGN_S") \
  F(std::string, em_fx_coll_dyn_align_s, , "EMFXCOLLDYNALIGN_S") \
  F(float, em_fx_lifespan, , "EMFXLIFESPAN") \
  F(int32_t, em_check_collision, , "EMCHECKCOLLISION") \
  F(int32_t, em_adjust_shp_to_origin, , "EMADJUSTSHPTOORIGIN") \
  F(float, em_invest_next_key_duration, , "EMINVESTNEXTKEYDURATION") \
  F(float, em_fly_gravity, , "EMFLYGRAVITY") \
  F(std::string, em_self_rot_vel_s, , "EMSELFROTVEL_S") \
  F(std::string, user_string, [user_string_count], "USERSTRING") \
  F(std::string, light_preset_name, , "LIGHTPRESETNAME") \
  F(std::string, sfx_id, , "SFXID") \
  F(int32_t, sfx_is_ambient, , "SFXISAMBIENT") \
  F(int32_t, send_assess_magic, , "SENDASSESSMAGIC") \
  F(float, secs_per_damage, , "SECSPERDAMAGE")

#define DEF_EFFECT_BASE_G2_FIELDS(F) \
  F(std::string, em_fx_coll_dyn_perc_s, , "EMFXCOLLDYNPERC_S")

#define DEF_EFFECT_BASE_FIELDS(F) \
  DEF_EFFECT_BASE_COMMON_FIELDS(F) \
  DEF_EFFECT_BASE_G2_FIELDS(F)

// CFX_BASE, 46 fields
struct EffectBase final {
  static constexpr uint8_t user_string_count = 5;

  DEF_EFFECT_BASE_FIELDS(DEF_DECLARE_FIELD)
  };

#define DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(F) \
  F(std::string, vis_name_s, , "VISNAME_S") \
  F(float, vis_size_scale, , "VISSIZESCALE") \
  F(float, scale_duration, , "SCALEDURATION") \
  F(float, pfx_pps_value, , "PFX_PPSVALUE") \
  F(int32_t, pfx_pps_is_smooth_chg, , "PFX_PPSISSMOOTHCHG") \
  F(int32_t, pfx_pps_is_looping_chg, , "PFX_PPSISLOOPINGCHG") \
  F(float, pfx_sc_time, , "PFX_SCTIME") \
  F(std::string, pfx_fly_gravity_s, , "PFX_FLYGRAVITY_S") \
  F(std::string, pfx_shp_dim_s, , "PFX_SHPDIM_S") \
  F(int32_t, pfx_shp_is_volume_chg, , "PFX_SHPISVOLUMECHG") \
  F(float, pfx_shp_scale_fps, , "PFX_SHPSCALEFPS") \
  F(float, pfx_shp_distrib_walks_peed, , "PFX_SHPDISTRIBWALKSPEED") \
  F(std::string, pfx_shp_offset_vec_s, , "PFX_SHPOFFSETVEC_S") \
  F(std::string, pfx_shp_distrib_type_s, , "PFX_SHPDISTRIBTYPE_S") \
  F(std::string, pfx_dir_mode_s, , "PFX_DIRMODE_S") \
  F(std::string, pfx_dir_for_s, , "PFX_DIRFOR_S") \
  F(std::string, pfx_dir_mode_target_for_s, , "PFX_DIRMODETARGETFOR_S") \
  F(std::string, pfx_dir_mode_target_pos_s, , "PFX_DIRMODETARGETPOS_S") \
  F(float, pfx_vel_avg, , "PFX_VELAVG") \
  F(float, pfx_lsp_part_avg, , "PFX_LSPPARTAVG") \
  F(float, pfx_vis_alpha_start, , "PFX_VISALPHASTART") \
  F(std::string, light_preset_name, , "LIGHTPRESETNAME") \
  F(float, light_range, , "LIGHTRANGE") \
  F(std::string, sfx_id, , "SFXID") \
  F(int32_t, sfx_is_ambient, , "SFXISAMBIENT") \
  F(std::string, em_create_fx_id, , "EMCREATEFXID") \
  F(float, em_fly_gravity, , "EMFLYGRAVITY") \
  F(std::string, em_self_rot_vel_s, , "EMSELFROTVEL_S") \
  F(std::string, em_trj_mode_s, , "EMTRJMODE_S") \
  F(float, em_trj_ease_vel, , "EMTRJEASEVEL") \
  F(int32_t, em_check_collision, , "EMCHECKCOLLISION") \
  F(float, em_fx_lifespan, , "EMFXLIFESPAN")

// C_PARTICLEFXEMITKEY, 32 fields
struct ParticleEffectEmitKey final {
  DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(DEF_DECLARE_FIELD)
  };

#undef DEF_DECLARE_FIELD

}
