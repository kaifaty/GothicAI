#include "definitiontypes.h"

#include <type_traits>

namespace Def {

static_assert(sizeof(FightMove)==4 && sizeof(MusicTransitionEffect)==4 && sizeof(MusicTransitionType)==4);
static_assert(uint32_t(FightMove::NOP)==0 && uint32_t(FightMove::RUN)==1 && uint32_t(FightMove::RUN_BACK)==2 &&
              uint32_t(FightMove::JUMP_BACK)==3 && uint32_t(FightMove::TURN)==4 && uint32_t(FightMove::STRAFE)==5 &&
              uint32_t(FightMove::ATTACK)==6 && uint32_t(FightMove::ATTACK_SIDE)==7 &&
              uint32_t(FightMove::ATTACK_FRONT)==8 && uint32_t(FightMove::ATTACK_TRIPLE)==9 &&
              uint32_t(FightMove::ATTACK_WHIRL)==10 && uint32_t(FightMove::ATTACK_MASTER)==11 &&
              uint32_t(FightMove::TURN_TO_HIT)==15 && uint32_t(FightMove::PARRY)==17 &&
              uint32_t(FightMove::STAND_UP)==18 && uint32_t(FightMove::WAIT)==19 &&
              uint32_t(FightMove::WAIT_LONGER)==23 && uint32_t(FightMove::WAIT_EXT)==24);
static_assert(uint32_t(MusicTransitionEffect::UNKNOWN)==0 && uint32_t(MusicTransitionEffect::NONE)==1 &&
              uint32_t(MusicTransitionEffect::GROOVE)==2 && uint32_t(MusicTransitionEffect::FILL)==3 &&
              uint32_t(MusicTransitionEffect::BREAK)==4 && uint32_t(MusicTransitionEffect::INTRO)==5 &&
              uint32_t(MusicTransitionEffect::END)==6 && uint32_t(MusicTransitionEffect::END_AND_INTO)==7);
static_assert(uint32_t(MusicTransitionType::UNKNOWN)==0 && uint32_t(MusicTransitionType::IMMEDIATE)==1 &&
              uint32_t(MusicTransitionType::BEAT)==2 && uint32_t(MusicTransitionType::MEASURE)==3);
static_assert(FightAi::move_count==6 && EffectBase::user_string_count==5);
static_assert(std::is_same_v<decltype(FightAi::move_count),const uint32_t> &&
              std::is_same_v<decltype(EffectBase::user_string_count),const uint8_t>);

void Camera::register_(zenkit::DaedalusScript& s) {
  s.register_member("CCAMSYS.BESTRANGE", &Camera::best_range);
  s.register_member("CCAMSYS.MINRANGE", &Camera::min_range);
  s.register_member("CCAMSYS.MAXRANGE", &Camera::max_range);
  s.register_member("CCAMSYS.BESTELEVATION", &Camera::best_elevation);
  s.register_member("CCAMSYS.MINELEVATION", &Camera::min_elevation);
  s.register_member("CCAMSYS.MAXELEVATION", &Camera::max_elevation);
  s.register_member("CCAMSYS.BESTAZIMUTH", &Camera::best_azimuth);
  s.register_member("CCAMSYS.MINAZIMUTH", &Camera::min_azimuth);
  s.register_member("CCAMSYS.MAXAZIMUTH", &Camera::max_azimuth);
  s.register_member("CCAMSYS.BESTROTZ", &Camera::best_rot_z);
  s.register_member("CCAMSYS.MINROTZ", &Camera::min_rot_z);
  s.register_member("CCAMSYS.MAXROTZ", &Camera::max_rot_z);
  s.register_member("CCAMSYS.ROTOFFSETX", &Camera::rot_offset_x);
  s.register_member("CCAMSYS.ROTOFFSETY", &Camera::rot_offset_y);
  s.register_member("CCAMSYS.ROTOFFSETZ", &Camera::rot_offset_z);
  s.register_member("CCAMSYS.TARGETOFFSETX", &Camera::target_offset_x);
  s.register_member("CCAMSYS.TARGETOFFSETY", &Camera::target_offset_y);
  s.register_member("CCAMSYS.TARGETOFFSETZ", &Camera::target_offset_z);
  s.register_member("CCAMSYS.VELOTRANS", &Camera::velo_trans);
  s.register_member("CCAMSYS.VELOROT", &Camera::velo_rot);
  s.register_member("CCAMSYS.TRANSLATE", &Camera::translate);
  s.register_member("CCAMSYS.ROTATE", &Camera::rotate);
  s.register_member("CCAMSYS.COLLISION", &Camera::collision);
  }

void SoundEffect::register_(zenkit::DaedalusScript& s) {
  s.register_member("C_SFX.FILE", &SoundEffect::file);
  s.register_member("C_SFX.PITCHOFF", &SoundEffect::pitch_off);
  s.register_member("C_SFX.PITCHVAR", &SoundEffect::pitch_var);
  s.register_member("C_SFX.VOL", &SoundEffect::vol);
  s.register_member("C_SFX.LOOP", &SoundEffect::loop);
  s.register_member("C_SFX.LOOPSTARTOFFSET", &SoundEffect::loop_start_offset);
  s.register_member("C_SFX.LOOPENDOFFSET", &SoundEffect::loop_end_offset);
  s.register_member("C_SFX.REVERBLEVEL", &SoundEffect::reverb_level);
  s.register_member("C_SFX.PFXNAME", &SoundEffect::pfx_name);
  }

void MusicTheme::register_(zenkit::DaedalusScript& s) {
  s.register_member("C_MUSICTHEME.FILE", &MusicTheme::file);
  s.register_member("C_MUSICTHEME.VOL", &MusicTheme::vol);
  s.register_member("C_MUSICTHEME.LOOP", &MusicTheme::loop);
  s.register_member("C_MUSICTHEME.REVERBMIX", &MusicTheme::reverbmix);
  s.register_member("C_MUSICTHEME.REVERBTIME", &MusicTheme::reverbtime);
  s.register_member("C_MUSICTHEME.TRANSTYPE", &MusicTheme::transtype);
  s.register_member("C_MUSICTHEME.TRANSSUBTYPE", &MusicTheme::transsubtype);
  }

void ParticleEffect::register_(zenkit::DaedalusScript& s) {
  s.register_member("C_PARTICLEFX.PPSVALUE", &ParticleEffect::pps_value);
  s.register_member("C_PARTICLEFX.PPSSCALEKEYS_S", &ParticleEffect::pps_scale_keys_s);
  s.register_member("C_PARTICLEFX.PPSISLOOPING", &ParticleEffect::pps_is_looping);
  s.register_member("C_PARTICLEFX.PPSISSMOOTH", &ParticleEffect::pps_is_smooth);
  s.register_member("C_PARTICLEFX.PPSFPS", &ParticleEffect::pps_fps);
  s.register_member("C_PARTICLEFX.PPSCREATEEM_S", &ParticleEffect::pps_create_em_s);
  s.register_member("C_PARTICLEFX.PPSCREATEEMDELAY", &ParticleEffect::pps_create_em_delay);
  s.register_member("C_PARTICLEFX.SHPTYPE_S", &ParticleEffect::shp_type_s);
  s.register_member("C_PARTICLEFX.SHPFOR_S", &ParticleEffect::shp_for_s);
  s.register_member("C_PARTICLEFX.SHPOFFSETVEC_S", &ParticleEffect::shp_offset_vec_s);
  s.register_member("C_PARTICLEFX.SHPDISTRIBTYPE_S", &ParticleEffect::shp_distrib_type_s);
  s.register_member("C_PARTICLEFX.SHPDISTRIBWALKSPEED", &ParticleEffect::shp_distrib_walk_speed);
  s.register_member("C_PARTICLEFX.SHPISVOLUME", &ParticleEffect::shp_is_volume);
  s.register_member("C_PARTICLEFX.SHPDIM_S", &ParticleEffect::shp_dim_s);
  s.register_member("C_PARTICLEFX.SHPMESH_S", &ParticleEffect::shp_mesh_s);
  s.register_member("C_PARTICLEFX.SHPMESHRENDER_B", &ParticleEffect::shp_mesh_render_b);
  s.register_member("C_PARTICLEFX.SHPSCALEKEYS_S", &ParticleEffect::shp_scale_keys_s);
  s.register_member("C_PARTICLEFX.SHPSCALEISLOOPING", &ParticleEffect::shp_scale_is_looping);
  s.register_member("C_PARTICLEFX.SHPSCALEISSMOOTH", &ParticleEffect::shp_scale_is_smooth);
  s.register_member("C_PARTICLEFX.SHPSCALEFPS", &ParticleEffect::shp_scale_fps);
  s.register_member("C_PARTICLEFX.DIRMODE_S", &ParticleEffect::dir_mode_s);
  s.register_member("C_PARTICLEFX.DIRFOR_S", &ParticleEffect::dir_for_s);
  s.register_member("C_PARTICLEFX.DIRMODETARGETFOR_S", &ParticleEffect::dir_mode_target_for_s);
  s.register_member("C_PARTICLEFX.DIRMODETARGETPOS_S", &ParticleEffect::dir_mode_target_pos_s);
  s.register_member("C_PARTICLEFX.DIRANGLEHEAD", &ParticleEffect::dir_angle_head);
  s.register_member("C_PARTICLEFX.DIRANGLEHEADVAR", &ParticleEffect::dir_angle_head_var);
  s.register_member("C_PARTICLEFX.DIRANGLEELEV", &ParticleEffect::dir_angle_elev);
  s.register_member("C_PARTICLEFX.DIRANGLEELEVVAR", &ParticleEffect::dir_angle_elev_var);
  s.register_member("C_PARTICLEFX.VELAVG", &ParticleEffect::vel_avg);
  s.register_member("C_PARTICLEFX.VELVAR", &ParticleEffect::vel_var);
  s.register_member("C_PARTICLEFX.LSPPARTAVG", &ParticleEffect::lsp_part_avg);
  s.register_member("C_PARTICLEFX.LSPPARTVAR", &ParticleEffect::lsp_part_var);
  s.register_member("C_PARTICLEFX.FLYGRAVITY_S", &ParticleEffect::fly_gravity_s);
  s.register_member("C_PARTICLEFX.FLYCOLLDET_B", &ParticleEffect::fly_colldet_b);
  s.register_member("C_PARTICLEFX.VISNAME_S", &ParticleEffect::vis_name_s);
  s.register_member("C_PARTICLEFX.VISORIENTATION_S", &ParticleEffect::vis_orientation_s);
  s.register_member("C_PARTICLEFX.VISTEXISQUADPOLY", &ParticleEffect::vis_tex_is_quadpoly);
  s.register_member("C_PARTICLEFX.VISTEXANIFPS", &ParticleEffect::vis_tex_ani_fps);
  s.register_member("C_PARTICLEFX.VISTEXANIISLOOPING", &ParticleEffect::vis_tex_ani_is_looping);
  s.register_member("C_PARTICLEFX.VISTEXCOLORSTART_S", &ParticleEffect::vis_tex_color_start_s);
  s.register_member("C_PARTICLEFX.VISTEXCOLOREND_S", &ParticleEffect::vis_tex_color_end_s);
  s.register_member("C_PARTICLEFX.VISSIZESTART_S", &ParticleEffect::vis_size_start_s);
  s.register_member("C_PARTICLEFX.VISSIZEENDSCALE", &ParticleEffect::vis_size_end_scale);
  s.register_member("C_PARTICLEFX.VISALPHAFUNC_S", &ParticleEffect::vis_alpha_func_s);
  s.register_member("C_PARTICLEFX.VISALPHASTART", &ParticleEffect::vis_alpha_start);
  s.register_member("C_PARTICLEFX.VISALPHAEND", &ParticleEffect::vis_alpha_end);
  s.register_member("C_PARTICLEFX.TRLFADESPEED", &ParticleEffect::trl_fade_speed);
  s.register_member("C_PARTICLEFX.TRLTEXTURE_S", &ParticleEffect::trl_texture_s);
  s.register_member("C_PARTICLEFX.TRLWIDTH", &ParticleEffect::trl_width);
  s.register_member("C_PARTICLEFX.MRKFADESPEED", &ParticleEffect::mrk_fades_peed);
  s.register_member("C_PARTICLEFX.MRKTEXTURE_S", &ParticleEffect::mrkt_exture_s);
  s.register_member("C_PARTICLEFX.MRKSIZE", &ParticleEffect::mrk_size);

  // Gothic 2 only
  if(s.find_symbol_by_name("C_PARTICLEFX.FLOCKMODE") != nullptr) {
    s.register_member("C_PARTICLEFX.FLOCKMODE", &ParticleEffect::flock_mode);
    s.register_member("C_PARTICLEFX.FLOCKSTRENGTH", &ParticleEffect::flock_strength);
    s.register_member("C_PARTICLEFX.USEEMITTERSFOR", &ParticleEffect::use_emitters_for);
    s.register_member("C_PARTICLEFX.TIMESTARTEND_S", &ParticleEffect::time_start_end_s);
    s.register_member("C_PARTICLEFX.M_BISAMBIENTPFX", &ParticleEffect::m_bis_ambient_pfx);
    }
  }

void EffectBase::register_(zenkit::DaedalusScript& s) {
  s.register_member("CFX_BASE.VISNAME_S", &EffectBase::vis_name_s);
  s.register_member("CFX_BASE.VISSIZE_S", &EffectBase::vis_size_s);
  s.register_member("CFX_BASE.VISALPHA", &EffectBase::vis_alpha);
  s.register_member("CFX_BASE.VISALPHABLENDFUNC_S", &EffectBase::vis_alpha_blend_func_s);
  s.register_member("CFX_BASE.VISTEXANIFPS", &EffectBase::vis_tex_ani_fps);
  s.register_member("CFX_BASE.VISTEXANIISLOOPING", &EffectBase::vis_tex_ani_is_looping);
  s.register_member("CFX_BASE.EMTRJMODE_S", &EffectBase::em_trj_mode_s);
  s.register_member("CFX_BASE.EMTRJORIGINNODE", &EffectBase::em_trj_origin_node);
  s.register_member("CFX_BASE.EMTRJTARGETNODE", &EffectBase::em_trj_target_node);
  s.register_member("CFX_BASE.EMTRJTARGETRANGE", &EffectBase::em_trj_target_range);
  s.register_member("CFX_BASE.EMTRJTARGETAZI", &EffectBase::em_trj_target_azi);
  s.register_member("CFX_BASE.EMTRJTARGETELEV", &EffectBase::em_trj_target_elev);
  s.register_member("CFX_BASE.EMTRJNUMKEYS", &EffectBase::em_trj_num_keys);
  s.register_member("CFX_BASE.EMTRJNUMKEYSVAR", &EffectBase::em_trj_num_keys_var);
  s.register_member("CFX_BASE.EMTRJANGLEELEVVAR", &EffectBase::em_trj_angle_elev_var);
  s.register_member("CFX_BASE.EMTRJANGLEHEADVAR", &EffectBase::em_trj_angle_head_var);
  s.register_member("CFX_BASE.EMTRJKEYDISTVAR", &EffectBase::em_trj_key_dist_var);
  s.register_member("CFX_BASE.EMTRJLOOPMODE_S", &EffectBase::em_trj_loop_mode_s);
  s.register_member("CFX_BASE.EMTRJEASEFUNC_S", &EffectBase::em_trj_ease_func_s);
  s.register_member("CFX_BASE.EMTRJEASEVEL", &EffectBase::em_trj_ease_vel);
  s.register_member("CFX_BASE.EMTRJDYNUPDATEDELAY", &EffectBase::em_trj_dyn_update_delay);
  s.register_member("CFX_BASE.EMTRJDYNUPDATETARGETONLY", &EffectBase::em_trj_dyn_update_target_only);
  s.register_member("CFX_BASE.EMFXCREATE_S", &EffectBase::em_fx_create_s);
  s.register_member("CFX_BASE.EMFXINVESTORIGIN_S", &EffectBase::em_fx_invest_origin_s);
  s.register_member("CFX_BASE.EMFXINVESTTARGET_S", &EffectBase::em_fx_invest_target_s);
  s.register_member("CFX_BASE.EMFXTRIGGERDELAY", &EffectBase::em_fx_trigger_delay);
  s.register_member("CFX_BASE.EMFXCREATEDOWNTRJ", &EffectBase::em_fx_create_down_trj);
  s.register_member("CFX_BASE.EMACTIONCOLLDYN_S", &EffectBase::em_action_coll_dyn_s);
  s.register_member("CFX_BASE.EMACTIONCOLLSTAT_S", &EffectBase::em_action_coll_stat_s);
  s.register_member("CFX_BASE.EMFXCOLLSTAT_S", &EffectBase::em_fx_coll_stat_s);
  s.register_member("CFX_BASE.EMFXCOLLDYN_S", &EffectBase::em_fx_coll_dyn_s);
  s.register_member("CFX_BASE.EMFXCOLLSTATALIGN_S", &EffectBase::em_fx_coll_stat_align_s);
  s.register_member("CFX_BASE.EMFXCOLLDYNALIGN_S", &EffectBase::em_fx_coll_dyn_align_s);
  s.register_member("CFX_BASE.EMFXLIFESPAN", &EffectBase::em_fx_lifespan);
  s.register_member("CFX_BASE.EMCHECKCOLLISION", &EffectBase::em_check_collision);
  s.register_member("CFX_BASE.EMADJUSTSHPTOORIGIN", &EffectBase::em_adjust_shp_to_origin);
  s.register_member("CFX_BASE.EMINVESTNEXTKEYDURATION", &EffectBase::em_invest_next_key_duration);
  s.register_member("CFX_BASE.EMFLYGRAVITY", &EffectBase::em_fly_gravity);
  s.register_member("CFX_BASE.EMSELFROTVEL_S", &EffectBase::em_self_rot_vel_s);
  s.register_member("CFX_BASE.USERSTRING", &EffectBase::user_string);
  s.register_member("CFX_BASE.LIGHTPRESETNAME", &EffectBase::light_preset_name);
  s.register_member("CFX_BASE.SFXID", &EffectBase::sfx_id);
  s.register_member("CFX_BASE.SFXISAMBIENT", &EffectBase::sfx_is_ambient);
  s.register_member("CFX_BASE.SENDASSESSMAGIC", &EffectBase::send_assess_magic);
  s.register_member("CFX_BASE.SECSPERDAMAGE", &EffectBase::secs_per_damage);

  // Gothic 2 only
  if(s.find_symbol_by_name("CFX_BASE.EMFXCOLLDYNPERC_S") != nullptr)
    s.register_member("CFX_BASE.EMFXCOLLDYNPERC_S", &EffectBase::em_fx_coll_dyn_perc_s);
  }

void ParticleEffectEmitKey::register_(zenkit::DaedalusScript& s) {
  s.register_member("C_PARTICLEFXEMITKEY.VISNAME_S", &ParticleEffectEmitKey::vis_name_s);
  s.register_member("C_PARTICLEFXEMITKEY.VISSIZESCALE", &ParticleEffectEmitKey::vis_size_scale);
  s.register_member("C_PARTICLEFXEMITKEY.SCALEDURATION", &ParticleEffectEmitKey::scale_duration);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_PPSVALUE", &ParticleEffectEmitKey::pfx_pps_value);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_PPSISSMOOTHCHG", &ParticleEffectEmitKey::pfx_pps_is_smooth_chg);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_PPSISLOOPINGCHG", &ParticleEffectEmitKey::pfx_pps_is_looping_chg);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SCTIME", &ParticleEffectEmitKey::pfx_sc_time);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_FLYGRAVITY_S", &ParticleEffectEmitKey::pfx_fly_gravity_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPDIM_S", &ParticleEffectEmitKey::pfx_shp_dim_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPISVOLUMECHG", &ParticleEffectEmitKey::pfx_shp_is_volume_chg);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPSCALEFPS", &ParticleEffectEmitKey::pfx_shp_scale_fps);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPDISTRIBWALKSPEED", &ParticleEffectEmitKey::pfx_shp_distrib_walks_peed);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPOFFSETVEC_S", &ParticleEffectEmitKey::pfx_shp_offset_vec_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_SHPDISTRIBTYPE_S", &ParticleEffectEmitKey::pfx_shp_distrib_type_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_DIRMODE_S", &ParticleEffectEmitKey::pfx_dir_mode_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_DIRFOR_S", &ParticleEffectEmitKey::pfx_dir_for_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_DIRMODETARGETFOR_S", &ParticleEffectEmitKey::pfx_dir_mode_target_for_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_DIRMODETARGETPOS_S", &ParticleEffectEmitKey::pfx_dir_mode_target_pos_s);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_VELAVG", &ParticleEffectEmitKey::pfx_vel_avg);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_LSPPARTAVG", &ParticleEffectEmitKey::pfx_lsp_part_avg);
  s.register_member("C_PARTICLEFXEMITKEY.PFX_VISALPHASTART", &ParticleEffectEmitKey::pfx_vis_alpha_start);
  s.register_member("C_PARTICLEFXEMITKEY.LIGHTPRESETNAME", &ParticleEffectEmitKey::light_preset_name);
  s.register_member("C_PARTICLEFXEMITKEY.LIGHTRANGE", &ParticleEffectEmitKey::light_range);
  s.register_member("C_PARTICLEFXEMITKEY.SFXID", &ParticleEffectEmitKey::sfx_id);
  s.register_member("C_PARTICLEFXEMITKEY.SFXISAMBIENT", &ParticleEffectEmitKey::sfx_is_ambient);
  s.register_member("C_PARTICLEFXEMITKEY.EMCREATEFXID", &ParticleEffectEmitKey::em_create_fx_id);
  s.register_member("C_PARTICLEFXEMITKEY.EMFLYGRAVITY", &ParticleEffectEmitKey::em_fly_gravity);
  s.register_member("C_PARTICLEFXEMITKEY.EMSELFROTVEL_S", &ParticleEffectEmitKey::em_self_rot_vel_s);
  s.register_member("C_PARTICLEFXEMITKEY.EMTRJMODE_S", &ParticleEffectEmitKey::em_trj_mode_s);
  s.register_member("C_PARTICLEFXEMITKEY.EMTRJEASEVEL", &ParticleEffectEmitKey::em_trj_ease_vel);
  s.register_member("C_PARTICLEFXEMITKEY.EMCHECKCOLLISION", &ParticleEffectEmitKey::em_check_collision);
  s.register_member("C_PARTICLEFXEMITKEY.EMFXLIFESPAN", &ParticleEffectEmitKey::em_fx_lifespan);
  }

}
