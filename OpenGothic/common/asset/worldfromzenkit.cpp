#include "fromzenkit.h"
#include "gothicglb.h"
#include <zenkit/Archive.hh>
#include <zenkit/CutsceneLibrary.hh>
#include <zenkit/Stream.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/Camera.hh>
#include <zenkit/vobs/Light.hh>
#include <zenkit/vobs/Misc.hh>
#include <zenkit/vobs/MovableObject.hh>
#include <zenkit/vobs/Sound.hh>
#include <zenkit/vobs/Trigger.hh>
#include <zenkit/vobs/Zone.hh>
#include <algorithm>
#include <bit>
#include <functional>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>

namespace WorldData {
namespace {
using namespace zenkit;
namespace fs = std::filesystem;
[[noreturn]] void fail(const std::string& reason) { throw std::runtime_error("world data: "+reason); }
void require(bool ok, const std::string& reason) { if(!ok) fail(reason); }

#define WORLD_DATA_OBJECTS(X) \
  X(oCNpcTalent,VNpc::Talent) \
  X(zCEventManager,EventManager) \
  X(zCDecal,VisualDecal) \
  X(zCMesh,VisualMesh) \
  X(zCProgMeshProto,VisualMultiResolutionMesh) \
  X(zCParticleFX,VisualParticleEffect) \
  X(zCAICamera,VisualCamera) \
  X(zCModel,VisualModel) \
  X(zCMorphMesh,VisualMorphMesh) \
  X(oCAIHuman,AiHuman) \
  X(oCAIVobMove,AiMove) \
  X(zCVobLevelCompo,VLevel) \
  X(zCVobStartpoint,VStartPoint) \
  X(zCVobStair,VStair) \
  X(zCVobSpot,VSpot) \
  X(zCVob,VirtualObject) \
  X(zCVobScreenFX,VScreenEffect) \
  X(zCCSCamera,VCutsceneCamera) \
  X(zCCamTrj_KeyFrame,VCameraTrajectoryFrame) \
  X(zCVobAnimate,VAnimate) \
  X(zCZoneVobFarPlane,VZoneFarPlane) \
  X(zCZoneVobFarPlaneDefault,VZoneFarPlaneDefault) \
  X(zCZoneZFogDefault,VZoneFogDefault) \
  X(zCZoneZFog,VZoneFog) \
  X(zCVobLensFlare,VLensFlare) \
  X(oCItem,VItem) \
  X(zCTrigger,VTrigger) \
  X(oCCSTrigger,VCutsceneTrigger) \
  X(oCMOB,VMovableObject) \
  X(oCMobInter,VInteractiveObject) \
  X(oCMobLadder,VLadder) \
  X(oCMobSwitch,VSwitch) \
  X(oCMobWheel,VWheel) \
  X(oCMobBed,VBed) \
  X(oCMobFire,VFire) \
  X(oCMobContainer,VContainer) \
  X(oCMobDoor,VDoor) \
  X(zCPFXController,VParticleEffectController) \
  X(zCVobLight,VLight) \
  X(zCVobSound,VSound) \
  X(zCVobSoundDaytime,VSoundDaytime) \
  X(oCZoneMusic,VZoneMusic) \
  X(oCZoneMusicDefault,VZoneMusicDefault) \
  X(zCMessageFilter,VMessageFilter) \
  X(zCCodeMaster,VCodeMaster) \
  X(zCTriggerList,VTriggerList) \
  X(oCTriggerScript,VTriggerScript) \
  X(zCMover,VMover) \
  X(oCTriggerChangeLevel,VTriggerChangeLevel) \
  X(zCTriggerWorldStart,VTriggerWorldStart) \
  X(oCTouchDamage,VTouchDamage) \
  X(zCTriggerUntouch,VTriggerUntouch) \
  X(zCEarthquake,VEarthquake) \
  X(zCMoverController,VMoverController) \
  X(oCNpc,VNpc) \
  X(zCCutscene,Cutscene) \
  X(zCCSCutsceneContext,CutsceneContext) \
  X(zCCSBlock,CutsceneBlock) \
  X(zCCSAtomicBlock,CutsceneAtomicBlock) \
  X(oCMsgConversation,ConversationMessageEvent) \
  X(zCCSProps,CutsceneProps) \
  X(zCWayNet,WayNet) \
  X(zCWaypoint,WayPoint)

// Both directions exist only at the archive boundary. The reverse path checks
// the import against ZenKit's independent ASCII writer, including graph aliases.
template<bool ToArchive = false> struct WorldCopy {
  using Source = std::conditional_t<ToArchive,asset::Object,zenkit::Object>;
  using Target = std::conditional_t<ToArchive,zenkit::Object,asset::Object>;
  std::unordered_map<const Source*,std::shared_ptr<Target>> objects;

  template<class A, class Z> static auto& source(A& a, Z& z) {
    if constexpr(ToArchive) return a; else return z;
    }
  template<class A, class Z> static auto& target(A& a, Z& z) {
    if constexpr(ToArchive) return z; else return a;
    }
  template<class A, class Z> void field(A& a, Z& z) {
    if constexpr(std::is_enum_v<A> && std::is_enum_v<Z>) {
      if constexpr(ToArchive) z=static_cast<Z>(a); else a=static_cast<A>(z);
      }
    else if constexpr(std::is_same_v<A,Z>) target(a,z)=source(a,z);
    else copy(a,z);
    }
  template<class A, class Z> void field(std::vector<A>& a, std::vector<Z>& z) {
    target(a,z).resize(source(a,z).size());
    for(size_t i=0;i<a.size();++i) field(a[i],z[i]);
    }
  template<class A, class Z, size_t N> void field(A (&a)[N], Z (&z)[N]) {
    for(size_t i=0;i<N;++i) field(a[i],z[i]);
    }
  template<class A, class Z> void field(std::optional<A>& a, std::optional<Z>& z) {
    if(!source(a,z)) target(a,z).reset();
    else { target(a,z).emplace(); field(*a,*z); }
    }
  template<class A, class Z> void field(std::unique_ptr<A>& a, std::unique_ptr<Z>& z) {
    if(!source(a,z)) { target(a,z).reset(); return; }
    if constexpr(ToArchive) z=std::make_unique<Z>(); else a=std::make_unique<A>();
    field(*a,*z);
    }
  template<class A, class Z> void field(std::weak_ptr<A>& a, std::weak_ptr<Z>& z) {
    auto ap=a.lock(); auto zp=z.lock(); field(ap,zp);
    if constexpr(ToArchive) z=zp; else a=ap;
    }
  template<class A, class Z> void field(std::shared_ptr<A>& a, std::shared_ptr<Z>& z) {
    const auto& s=source(a,z);
    if(!s) { target(a,z).reset(); return; }
    auto it=objects.find(s.get());
    if(it==objects.end()) {
      std::shared_ptr<Target> p;
      switch(int(s->get_object_type())) {
#define MAKE(tag,cpp) case int(zenkit::ObjectType::tag): \
        static_assert(int(asset::ObjectType::tag)==int(zenkit::ObjectType::tag)); \
        if constexpr(ToArchive) p=std::make_shared<zenkit::cpp>(); else p=std::make_shared<asset::cpp>(); break;
        WORLD_DATA_OBJECTS(MAKE)
#undef MAKE
        default:
          if constexpr(ToArchive) {
            require(typeid(*s)==typeid(asset::Visual),"unsupported own world object");
            p=std::make_shared<zenkit::Visual>();
            } else {
            require(typeid(*s)==typeid(zenkit::Visual),"unsupported archive world object");
            p=std::make_shared<asset::Visual>();
            }
        }
      // Register before copying fields: children, AI and cutscenes can refer back.
      objects.emplace(s.get(),p);
      if constexpr(ToArchive) z=std::dynamic_pointer_cast<Z>(p); else a=std::dynamic_pointer_cast<A>(p);
      require(bool(target(a,z)),"world object reference has the wrong class");
      object(*a,*z);
      } else {
      if constexpr(ToArchive) z=std::dynamic_pointer_cast<Z>(it->second); else a=std::dynamic_pointer_cast<A>(it->second);
      require(bool(target(a,z)),"world object reference has the wrong class");
      }
    }
  template<class A, class B, class Z, class W> void field(std::pair<A,B>& a, std::pair<Z,W>& z) {
    field(a.first,z.first); field(a.second,z.second);
    }
  void field(std::variant<std::shared_ptr<asset::CutsceneAtomicBlock>,std::shared_ptr<asset::CutsceneBlock>>& a,
             std::variant<std::shared_ptr<zenkit::CutsceneAtomicBlock>,std::shared_ptr<zenkit::CutsceneBlock>>& z) {
    if(source(a,z).index()==0) {
      target(a,z).template emplace<0>(); field(std::get<0>(a),std::get<0>(z));
      } else {
      target(a,z).template emplace<1>(); field(std::get<1>(a),std::get<1>(z));
      }
    }
  void object(asset::Object& a, zenkit::Object& z) {
    switch(z.get_object_type()) {
#define COPY(tag,cpp) case zenkit::ObjectType::tag: { \
        auto* ap=dynamic_cast<asset::cpp*>(&a); auto* zp=dynamic_cast<zenkit::cpp*>(&z); \
        require(ap && zp,"world class/type mismatch: " #tag); copy(*ap,*zp); return; }
      WORLD_DATA_OBJECTS(COPY)
#undef COPY
      default:
        if(typeid(a)==typeid(asset::Visual) && typeid(z)==typeid(zenkit::Visual)) {
          copy(static_cast<asset::Visual&>(a),static_cast<zenkit::Visual&>(z)); return;
          }
        fail("unsupported world object");
      }
    }
  void copy(asset::MoverKeyframe& a, zenkit::AnimationSample& z) {
    field(a.position,z.position);
    field(a.rotation,z.rotation);
    }
  void copy(asset::RigidBody& a, zenkit::RigidBody& z) {
    field(a.vel,z.vel);
    field(a.mode,z.mode);
    field(a.gravity_enabled,z.gravity_enabled);
    field(a.gravity_scale,z.gravity_scale);
    field(a.slide_direction,z.slide_direction);
    }
  void copy(asset::Visual& a, zenkit::Visual& z) {
    field(a.type,z.type);
    field(a.name,z.name);
    }
  void copy(asset::VisualDecal& a, zenkit::VisualDecal& z) {
    copy(static_cast<asset::Visual&>(a),static_cast<zenkit::Visual&>(z));
    field(a.dimension,z.dimension);
    field(a.offset,z.offset);
    field(a.two_sided,z.two_sided);
    field(a.alpha_func,z.alpha_func);
    field(a.texture_anim_fps,z.texture_anim_fps);
    field(a.alpha_weight,z.alpha_weight);
    field(a.ignore_daylight,z.ignore_daylight);
    }
  void copy(asset::VirtualObject& a, zenkit::VirtualObject& z) {
    field(a.type,z.type);
    field(a.id,z.id);
    field(a.bbox,z.bbox);
    field(a.position,z.position);
    field(a.rotation,z.rotation);
    field(a.show_visual,z.show_visual);
    field(a.sprite_camera_facing_mode,z.sprite_camera_facing_mode);
    field(a.cd_static,z.cd_static);
    field(a.cd_dynamic,z.cd_dynamic);
    field(a.vob_static,z.vob_static);
    field(a.dynamic_shadows,z.dynamic_shadows);
    field(a.physics_enabled,z.physics_enabled);
    field(a.anim_mode,z.anim_mode);
    field(a.bias,z.bias);
    field(a.ambient,z.ambient);
    field(a.anim_strength,z.anim_strength);
    field(a.far_clip_scale,z.far_clip_scale);
    field(a.preset_name,z.preset_name);
    field(a.vob_name,z.vob_name);
    field(a.visual_name,z.visual_name);
    field(a.associated_visual_type,z.associated_visual_type);
    field(a.visual_decal,z.visual_decal);
    field(a.visual,z.visual);
    field(a.ai,z.ai);
    field(a.event_manager,z.event_manager);
    field(a.sleep_mode,z.sleep_mode);
    field(a.next_on_timer,z.next_on_timer);
    field(a.rigid_body,z.rigid_body);
    field(a.children,z.children);
    }
  void copy(asset::EventManager& a, zenkit::EventManager& z) {
    field(a.cleared,z.cleared);
    field(a.active,z.active);
    field(a.cutscene,z.cutscene);
    }
  void copy(asset::AiHuman& a, zenkit::AiHuman& z) {
    field(a.water_level,z.water_level);
    field(a.floor_y,z.floor_y);
    field(a.water_y,z.water_y);
    field(a.ceil_y,z.ceil_y);
    field(a.feet_y,z.feet_y);
    field(a.head_y,z.head_y);
    field(a.fall_dist_y,z.fall_dist_y);
    field(a.fall_start_y,z.fall_start_y);
    field(a.npc,z.npc);
    field(a.walk_mode,z.walk_mode);
    field(a.weapon_mode,z.weapon_mode);
    field(a.wmode_ast,z.wmode_ast);
    field(a.wmode_select,z.wmode_select);
    field(a.change_weapon,z.change_weapon);
    field(a.action_mode,z.action_mode);
    }
  void copy(asset::AiMove& a, zenkit::AiMove& z) {
    field(a.vob,z.vob);
    field(a.owner,z.owner);
    }
  void copy(asset::VCameraTrajectoryFrame& a, zenkit::VCameraTrajectoryFrame& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.time,z.time);
    field(a.roll_angle,z.roll_angle);
    field(a.fov_scale,z.fov_scale);
    field(a.motion_type,z.motion_type);
    field(a.motion_type_fov,z.motion_type_fov);
    field(a.motion_type_roll,z.motion_type_roll);
    field(a.motion_type_time_scale,z.motion_type_time_scale);
    field(a.tension,z.tension);
    field(a.cam_bias,z.cam_bias);
    field(a.continuity,z.continuity);
    field(a.time_scale,z.time_scale);
    field(a.time_fixed,z.time_fixed);
    field(a.original_pose,z.original_pose);
    }
  void copy(asset::VCutsceneCamera& a, zenkit::VCutsceneCamera& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.trajectory_for,z.trajectory_for);
    field(a.target_trajectory_for,z.target_trajectory_for);
    field(a.loop_mode,z.loop_mode);
    field(a.lerp_mode,z.lerp_mode);
    field(a.ignore_for_vob_rotation,z.ignore_for_vob_rotation);
    field(a.ignore_for_vob_rotation_target,z.ignore_for_vob_rotation_target);
    field(a.adapt,z.adapt);
    field(a.ease_first,z.ease_first);
    field(a.ease_last,z.ease_last);
    field(a.total_duration,z.total_duration);
    field(a.auto_focus_vob,z.auto_focus_vob);
    field(a.auto_player_movable,z.auto_player_movable);
    field(a.auto_untrigger_last,z.auto_untrigger_last);
    field(a.auto_untrigger_last_delay,z.auto_untrigger_last_delay);
    field(a.position_count,z.position_count);
    field(a.target_count,z.target_count);
    field(a.trajectory_frames,z.trajectory_frames);
    field(a.target_frames,z.target_frames);
    field(a.paused,z.paused);
    field(a.started,z.started);
    field(a.goto_time_mode,z.goto_time_mode);
    field(a.cs_time,z.cs_time);
    }
  void copy(asset::LightPreset& a, zenkit::LightPreset& z) {
    field(a.preset,z.preset);
    field(a.light_type,z.light_type);
    field(a.range,z.range);
    field(a.color,z.color);
    field(a.cone_angle,z.cone_angle);
    field(a.is_static,z.is_static);
    field(a.quality,z.quality);
    field(a.lensflare_fx,z.lensflare_fx);
    field(a.on,z.on);
    field(a.range_animation_scale,z.range_animation_scale);
    field(a.range_animation_fps,z.range_animation_fps);
    field(a.range_animation_smooth,z.range_animation_smooth);
    field(a.color_animation_list,z.color_animation_list);
    field(a.color_animation_fps,z.color_animation_fps);
    field(a.color_animation_smooth,z.color_animation_smooth);
    field(a.can_move,z.can_move);
    }
  void copy(asset::VLight& a, zenkit::VLight& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    copy(static_cast<asset::LightPreset&>(a),static_cast<zenkit::LightPreset&>(z));
    }
  void copy(asset::VAnimate& a, zenkit::VAnimate& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.start_on,z.start_on);
    field(a.s_is_running,z.s_is_running);
    }
  void copy(asset::VItem& a, zenkit::VItem& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.instance,z.instance);
    field(a.s_amount,z.s_amount);
    field(a.s_flags,z.s_flags);
    }
  void copy(asset::VLensFlare& a, zenkit::VLensFlare& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.fx,z.fx);
    }
  void copy(asset::VParticleEffectController& a, zenkit::VParticleEffectController& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.pfx_name,z.pfx_name);
    field(a.kill_when_done,z.kill_when_done);
    field(a.initially_running,z.initially_running);
    }
  void copy(asset::VMessageFilter& a, zenkit::VMessageFilter& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.target,z.target);
    field(a.on_trigger,z.on_trigger);
    field(a.on_untrigger,z.on_untrigger);
    }
  void copy(asset::VCodeMaster& a, zenkit::VCodeMaster& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.target,z.target);
    field(a.ordered,z.ordered);
    field(a.first_false_is_failure,z.first_false_is_failure);
    field(a.failure_target,z.failure_target);
    field(a.untriggered_cancels,z.untriggered_cancels);
    field(a.slaves,z.slaves);
    field(a.s_num_triggered_slaves,z.s_num_triggered_slaves);
    }
  void copy(asset::VMoverController& a, zenkit::VMoverController& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.target,z.target);
    field(a.message,z.message);
    field(a.key,z.key);
    }
  void copy(asset::VTouchDamage& a, zenkit::VTouchDamage& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.damage,z.damage);
    field(a.barrier,z.barrier);
    field(a.blunt,z.blunt);
    field(a.edge,z.edge);
    field(a.fire,z.fire);
    field(a.fly,z.fly);
    field(a.magic,z.magic);
    field(a.point,z.point);
    field(a.fall,z.fall);
    field(a.repeat_delay_sec,z.repeat_delay_sec);
    field(a.volume_scale,z.volume_scale);
    field(a.collision,z.collision);
    }
  void copy(asset::VEarthquake& a, zenkit::VEarthquake& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.radius,z.radius);
    field(a.duration,z.duration);
    field(a.amplitude,z.amplitude);
    }
  void copy(asset::VNpc::Talent& a, zenkit::VNpc::Talent& z) {
    field(a.talent,z.talent);
    field(a.value,z.value);
    field(a.skill,z.skill);
    }
  void copy(asset::VNpc::Slot& a, zenkit::VNpc::Slot& z) {
    field(a.used,z.used);
    field(a.name,z.name);
    field(a.item,z.item);
    field(a.in_inventory,z.in_inventory);
    }
  void copy(asset::VNpc::News& a, zenkit::VNpc::News& z) {
    field(a.told,z.told);
    field(a.spread_time,z.spread_time);
    field(a.spread_type,z.spread_type);
    field(a.news_id,z.news_id);
    field(a.gossip,z.gossip);
    field(a.guild_victim,z.guild_victim);
    field(a.witness_name,z.witness_name);
    field(a.offender_name,z.offender_name);
    field(a.victim_name,z.victim_name);
    }
  void copy(asset::VNpc& a, zenkit::VNpc& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.npc_instance,z.npc_instance);
    field(a.model_scale,z.model_scale);
    field(a.model_fatness,z.model_fatness);
    field(a.overlays,z.overlays);
    field(a.flags,z.flags);
    field(a.guild,z.guild);
    field(a.guild_true,z.guild_true);
    field(a.level,z.level);
    field(a.xp,z.xp);
    field(a.xp_next_level,z.xp_next_level);
    field(a.lp,z.lp);
    field(a.talents,z.talents);
    field(a.fight_tactic,z.fight_tactic);
    field(a.fight_mode,z.fight_mode);
    field(a.wounded,z.wounded);
    field(a.mad,z.mad);
    field(a.mad_time,z.mad_time);
    field(a.player,z.player);
    field(a.attributes,z.attributes);
    field(a.hit_chance,z.hit_chance);
    field(a.missions,z.missions);
    field(a.start_ai_state,z.start_ai_state);
    field(a.aivar,z.aivar);
    field(a.script_waypoint,z.script_waypoint);
    field(a.attitude,z.attitude);
    field(a.attitude_temp,z.attitude_temp);
    field(a.name_nr,z.name_nr);
    field(a.move_lock,z.move_lock);
    field(a.packed,z.packed);
    field(a.news,z.news);
    field(a.items,z.items);
    field(a.slots,z.slots);
    field(a.current_state_valid,z.current_state_valid);
    field(a.current_state_name,z.current_state_name);
    field(a.current_state_index,z.current_state_index);
    field(a.current_state_is_routine,z.current_state_is_routine);
    field(a.next_state_valid,z.next_state_valid);
    field(a.next_state_name,z.next_state_name);
    field(a.next_state_index,z.next_state_index);
    field(a.next_state_is_routine,z.next_state_is_routine);
    field(a.last_ai_state,z.last_ai_state);
    field(a.has_routine,z.has_routine);
    field(a.routine_changed,z.routine_changed);
    field(a.routine_overlay,z.routine_overlay);
    field(a.routine_overlay_count,z.routine_overlay_count);
    field(a.walkmode_routine,z.walkmode_routine);
    field(a.weaponmode_routine,z.weaponmode_routine);
    field(a.start_new_routine,z.start_new_routine);
    field(a.ai_state_driven,z.ai_state_driven);
    field(a.ai_state_pos,z.ai_state_pos);
    field(a.current_routine,z.current_routine);
    field(a.respawn,z.respawn);
    field(a.respawn_time,z.respawn_time);
    field(a.protection,z.protection);
    field(a.bs_interruptable_override,z.bs_interruptable_override);
    field(a.npc_type,z.npc_type);
    field(a.spell_mana,z.spell_mana);
    field(a.carry_vob,z.carry_vob);
    field(a.enemy,z.enemy);
    }
  void copy(asset::VMovableObject& a, zenkit::VMovableObject& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.name,z.name);
    field(a.hp,z.hp);
    field(a.damage,z.damage);
    field(a.movable,z.movable);
    field(a.takable,z.takable);
    field(a.focus_override,z.focus_override);
    field(a.material,z.material);
    field(a.visual_destroyed,z.visual_destroyed);
    field(a.owner,z.owner);
    field(a.owner_guild,z.owner_guild);
    field(a.destroyed,z.destroyed);
    }
  void copy(asset::VInteractiveObject& a, zenkit::VInteractiveObject& z) {
    copy(static_cast<asset::VMovableObject&>(a),static_cast<zenkit::VMovableObject&>(z));
    field(a.state_count,z.state_count);
    field(a.target,z.target);
    field(a.item,z.item);
    field(a.condition_function,z.condition_function);
    field(a.on_state_change_function,z.on_state_change_function);
    field(a.rewind,z.rewind);
    }
  void copy(asset::VFire& a, zenkit::VFire& z) {
    copy(static_cast<asset::VInteractiveObject&>(a),static_cast<zenkit::VInteractiveObject&>(z));
    field(a.slot,z.slot);
    field(a.vob_tree,z.vob_tree);
    }
  void copy(asset::VContainer& a, zenkit::VContainer& z) {
    copy(static_cast<asset::VInteractiveObject&>(a),static_cast<zenkit::VInteractiveObject&>(z));
    field(a.locked,z.locked);
    field(a.key,z.key);
    field(a.pick_string,z.pick_string);
    field(a.contents,z.contents);
    field(a.s_items,z.s_items);
    }
  void copy(asset::VDoor& a, zenkit::VDoor& z) {
    copy(static_cast<asset::VInteractiveObject&>(a),static_cast<zenkit::VInteractiveObject&>(z));
    field(a.locked,z.locked);
    field(a.key,z.key);
    field(a.pick_string,z.pick_string);
    }
  void copy(asset::VSound& a, zenkit::VSound& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.volume,z.volume);
    field(a.mode,z.mode);
    field(a.random_delay,z.random_delay);
    field(a.random_delay_var,z.random_delay_var);
    field(a.initially_playing,z.initially_playing);
    field(a.ambient3d,z.ambient3d);
    field(a.obstruction,z.obstruction);
    field(a.cone_angle,z.cone_angle);
    field(a.volume_type,z.volume_type);
    field(a.radius,z.radius);
    field(a.sound_name,z.sound_name);
    field(a.s_is_running,z.s_is_running);
    field(a.s_is_allowed_to_run,z.s_is_allowed_to_run);
    }
  void copy(asset::VSoundDaytime& a, zenkit::VSoundDaytime& z) {
    copy(static_cast<asset::VSound&>(a),static_cast<zenkit::VSound&>(z));
    field(a.start_time,z.start_time);
    field(a.end_time,z.end_time);
    field(a.sound_name2,z.sound_name2);
    }
  void copy(asset::VTrigger& a, zenkit::VTrigger& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.flags,z.flags);
    field(a.filter_flags,z.filter_flags);
    field(a.target,z.target);
    field(a.start_enabled,z.start_enabled);
    field(a.send_untrigger,z.send_untrigger);
    field(a.react_to_on_trigger,z.react_to_on_trigger);
    field(a.react_to_on_touch,z.react_to_on_touch);
    field(a.react_to_on_damage,z.react_to_on_damage);
    field(a.respond_to_object,z.respond_to_object);
    field(a.respond_to_pc,z.respond_to_pc);
    field(a.respond_to_npc,z.respond_to_npc);
    field(a.vob_target,z.vob_target);
    field(a.max_activation_count,z.max_activation_count);
    field(a.retrigger_delay_sec,z.retrigger_delay_sec);
    field(a.damage_threshold,z.damage_threshold);
    field(a.fire_delay_sec,z.fire_delay_sec);
    field(a.s_next_time_triggerable,z.s_next_time_triggerable);
    field(a.s_count_can_be_activated,z.s_count_can_be_activated);
    field(a.s_other_vob,z.s_other_vob);
    field(a.s_is_enabled,z.s_is_enabled);
    }
  void copy(asset::VMover& a, zenkit::VMover& z) {
    copy(static_cast<asset::VTrigger&>(a),static_cast<zenkit::VTrigger&>(z));
    field(a.behavior,z.behavior);
    field(a.touch_blocker_damage,z.touch_blocker_damage);
    field(a.stay_open_time_sec,z.stay_open_time_sec);
    field(a.locked,z.locked);
    field(a.auto_link,z.auto_link);
    field(a.auto_rotate,z.auto_rotate);
    field(a.speed,z.speed);
    field(a.lerp_mode,z.lerp_mode);
    field(a.speed_mode,z.speed_mode);
    field(a.keyframes,z.keyframes);
    field(a.sfx_open_start,z.sfx_open_start);
    field(a.sfx_open_end,z.sfx_open_end);
    field(a.sfx_transitioning,z.sfx_transitioning);
    field(a.sfx_close_start,z.sfx_close_start);
    field(a.sfx_close_end,z.sfx_close_end);
    field(a.sfx_lock,z.sfx_lock);
    field(a.sfx_unlock,z.sfx_unlock);
    field(a.sfx_use_locked,z.sfx_use_locked);
    field(a.s_act_key_pos_delta,z.s_act_key_pos_delta);
    field(a.s_act_keyframe_f,z.s_act_keyframe_f);
    field(a.s_act_keyframe,z.s_act_keyframe);
    field(a.s_next_keyframe,z.s_next_keyframe);
    field(a.s_move_speed_unit,z.s_move_speed_unit);
    field(a.s_advance_dir,z.s_advance_dir);
    field(a.s_mover_state,z.s_mover_state);
    field(a.s_trigger_event_count,z.s_trigger_event_count);
    field(a.s_stay_open_time_dest,z.s_stay_open_time_dest);
    }
  void copy(asset::VTriggerList::Target& a, zenkit::VTriggerList::Target& z) {
    field(a.name,z.name);
    field(a.delay,z.delay);
    }
  void copy(asset::VTriggerList& a, zenkit::VTriggerList& z) {
    copy(static_cast<asset::VTrigger&>(a),static_cast<zenkit::VTrigger&>(z));
    field(a.mode,z.mode);
    field(a.targets,z.targets);
    field(a.s_act_target,z.s_act_target);
    field(a.s_send_on_trigger,z.s_send_on_trigger);
    }
  void copy(asset::VTriggerScript& a, zenkit::VTriggerScript& z) {
    copy(static_cast<asset::VTrigger&>(a),static_cast<zenkit::VTrigger&>(z));
    field(a.function,z.function);
    }
  void copy(asset::VTriggerChangeLevel& a, zenkit::VTriggerChangeLevel& z) {
    copy(static_cast<asset::VTrigger&>(a),static_cast<zenkit::VTrigger&>(z));
    field(a.level_name,z.level_name);
    field(a.start_vob,z.start_vob);
    }
  void copy(asset::VTriggerWorldStart& a, zenkit::VTriggerWorldStart& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.target,z.target);
    field(a.fire_once,z.fire_once);
    field(a.s_has_fired,z.s_has_fired);
    }
  void copy(asset::VTriggerUntouch& a, zenkit::VTriggerUntouch& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.target,z.target);
    }
  void copy(asset::VZoneMusic& a, zenkit::VZoneMusic& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.enabled,z.enabled);
    field(a.priority,z.priority);
    field(a.ellipsoid,z.ellipsoid);
    field(a.reverb,z.reverb);
    field(a.volume,z.volume);
    field(a.loop,z.loop);
    field(a.s_local_enabled,z.s_local_enabled);
    field(a.s_day_entrance_done,z.s_day_entrance_done);
    field(a.s_night_entrance_done,z.s_night_entrance_done);
    }
  void copy(asset::VZoneFarPlane& a, zenkit::VZoneFarPlane& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.vob_far_plane_z,z.vob_far_plane_z);
    field(a.inner_range_percentage,z.inner_range_percentage);
    }
  void copy(asset::VZoneFog& a, zenkit::VZoneFog& z) {
    copy(static_cast<asset::VirtualObject&>(a),static_cast<zenkit::VirtualObject&>(z));
    field(a.range_center,z.range_center);
    field(a.inner_range_percentage,z.inner_range_percentage);
    field(a.color,z.color);
    field(a.fade_out_sky,z.fade_out_sky);
    field(a.override_color,z.override_color);
    }
  void copy(asset::ConversationMessageEvent& a, zenkit::ConversationMessageEvent& z) {
    field(a.type,z.type);
    field(a.text,z.text);
    field(a.name,z.name);
    field(a.high_priority,z.high_priority);
    field(a.used,z.used);
    field(a.deleted,z.deleted);
    }
  void copy(asset::CutsceneAtomicBlock& a, zenkit::CutsceneAtomicBlock& z) {
    field(a.message,z.message);
    field(a.synchronized,z.synchronized);
    }
  void copy(asset::CutsceneBlock& a, zenkit::CutsceneBlock& z) {
    field(a.name,z.name);
    field(a.block,z.block);
    }
  void copy(asset::Cutscene& a, zenkit::Cutscene& z) {
    copy(static_cast<asset::CutsceneBlock&>(a),static_cast<zenkit::CutsceneBlock&>(z));
    }
  void copy(asset::CutsceneProps& a, zenkit::CutsceneProps& z) {
    field(a.name,z.name);
    field(a.global,z.global);
    field(a.loop,z.loop);
    field(a.has_to_be_triggered,z.has_to_be_triggered);
    field(a.distance,z.distance);
    field(a.range,z.range);
    field(a.locked_block_count,z.locked_block_count);
    field(a.run_behaviour,z.run_behaviour);
    field(a.run_behaviour_value,z.run_behaviour_value);
    field(a.stage_name,z.stage_name);
    field(a.script_function_on_stop,z.script_function_on_stop);
    }
  void copy(asset::CutsceneContext& a, zenkit::CutsceneContext& z) {
    copy(static_cast<asset::Cutscene&>(a),static_cast<zenkit::Cutscene&>(z));
    field(a.props,z.props);
    field(a.role_count,z.role_count);
    field(a.role_vob_count,z.role_vob_count);
    field(a.npc,z.npc);
    field(a.main_role,z.main_role);
    field(a.is_cutscene,z.is_cutscene);
    field(a.reference,z.reference);
    field(a.actual_block,z.actual_block);
    field(a.alt_stage,z.alt_stage);
    field(a.stage,z.stage);
    field(a.was_triggered,z.was_triggered);
    }
  void copy(asset::WayPoint& a, zenkit::WayPoint& z) {
    field(a.name,z.name);
    field(a.water_depth,z.water_depth);
    field(a.under_water,z.under_water);
    field(a.position,z.position);
    field(a.direction,z.direction);
    field(a.free_point,z.free_point);
    }
  void copy(asset::WayNet& a, zenkit::WayNet& z) {
    field(a.points,z.points);
    field(a.edges,z.edges);
    }
};

std::shared_ptr<zenkit::Object> makeOriginalObject(zenkit::ObjectType type) {
  switch(type) {
#define MAKE(tag,cpp) case ObjectType::tag: return std::make_shared<cpp>();
    WORLD_DATA_OBJECTS(MAKE)
#undef MAKE
    default: fail("unknown archive class");
    }
  }
std::string className(Object& v) {
  switch(v.get_object_type()) {
#define NAME(tag,cpp) case ObjectType::tag: return #tag;
    WORLD_DATA_OBJECTS(NAME)
#undef NAME
    default:
      if(typeid(v)==typeid(Visual)) return "Visual";
      fail("unknown archive class");
    }
  }

// ASCII archives carry wall-clock time. Only that provenance line is normalized;
// every object, field, reference and child count still comes from ZenKit itself.
std::string ascii(const std::function<void(WriteArchive&)>& save) {
  std::vector<std::byte> bytes;
  auto stream=Write::to(&bytes);
  auto archive=WriteArchive::to(stream.get(),ArchiveFormat::ASCII);
  save(*archive);
  archive->write_header();
  std::string text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  const auto date=text.find("\ndate ");
  require(date!=std::string::npos,"ZenKit ASCII header lacks date");
  text.replace(date+1,text.find('\n',date+1)-date-1,"date 1.1.2000 00:00:00");
  return text;
  }
std::string textDifference(const std::string& expected, const std::string& actual, const std::string& context) {
  if(expected==actual) return {};
  size_t at=0;
  while(at<std::min(expected.size(),actual.size()) && expected[at]==actual[at]) ++at;
  const auto line=[&](const std::string& s) {
    if(at>=s.size()) return std::string("<EOF>");
    const auto start=s.rfind('\n',at);
    return s.substr(start==std::string::npos ? 0 : start+1,s.find('\n',at)-(start==std::string::npos ? 0 : start+1));
    };
  auto object=expected.rfind('[',at);
  auto objectEnd=object==std::string::npos ? object : expected.find('\n',object);
  return context+": ZenKit ASCII byte "+std::to_string(at)+" "+
         (object==std::string::npos ? "" : expected.substr(object,objectEnd-object))+
         ", expected '"+line(expected)+"', actual '"+line(actual)+"'";
  }
void equalFloat(float a, float b, const std::string& where) {
  require(std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b),where+": expected "+
          std::to_string(a)+" (bits "+std::to_string(std::bit_cast<uint32_t>(a))+"), actual "+
          std::to_string(b)+" (bits "+std::to_string(std::bit_cast<uint32_t>(b))+")");
  }
void equalVec(const Vec3& a, const Vec3& b, const std::string& where) {
  for(unsigned i=0;i<3;++i) equalFloat(a[i],b[i],where+"["+std::to_string(i)+"]");
  }
template<class T> void equalInt(T a, T b, const std::string& where) {
  require(a==b,where+": expected "+std::to_string(a)+", actual "+std::to_string(b));
  }
void equalString(const std::string& a, const std::string& b, const std::string& where) {
  require(a==b,where+": expected '"+a+"', actual '"+b+"'");
  }
void compareLight(const LightPreset& a, const LightPreset& b, const std::string& where) {
  // Static presets omit their animation/on fields from ZenKit's archive.
  equalInt(a.on,b.on,where+"on");
  equalInt(a.can_move,b.can_move,where+"can_move");
  equalInt(a.range_animation_scale.size(),b.range_animation_scale.size(),where+"range_animation_scale.size");
  for(size_t i=0;i<a.range_animation_scale.size();++i)
    equalFloat(a.range_animation_scale[i],b.range_animation_scale[i],where+"range_animation_scale["+std::to_string(i)+"]");
  equalFloat(a.range_animation_fps,b.range_animation_fps,where+"range_animation_fps");
  equalInt(a.range_animation_smooth,b.range_animation_smooth,where+"range_animation_smooth");
  equalInt(a.color_animation_list.size(),b.color_animation_list.size(),where+"color_animation_list.size");
  for(size_t i=0;i<a.color_animation_list.size();++i) {
    const auto& x=a.color_animation_list[i]; const auto& y=b.color_animation_list[i];
    const auto field=where+"color_animation_list["+std::to_string(i)+"]";
    equalInt(x.r,y.r,field+".r"); equalInt(x.g,y.g,field+".g");
    equalInt(x.b,y.b,field+".b"); equalInt(x.a,y.a,field+".a");
    }
  equalFloat(a.color_animation_fps,b.color_animation_fps,where+"color_animation_fps");
  equalInt(a.color_animation_smooth,b.color_animation_smooth,where+"color_animation_smooth");
  }
void compareVobTree(const std::vector<std::shared_ptr<VirtualObject>>& a,
                    const std::vector<std::shared_ptr<VirtualObject>>& b, const std::string& path) {
  equalInt(a.size(),b.size(),path+" count");
  for(size_t i=0;i<a.size();++i) {
    const auto node=path+"["+std::to_string(i)+"]";
    require(a[i]!=nullptr && b[i]!=nullptr,node+": null vob");
    const auto& x=*a[i]; const auto& y=*b[i];
    const auto where=node+" "+className(*a[i])+" '"+x.vob_name+"' ";
    equalString(className(*a[i]),className(*b[i]),where+"class");
    // Vob dispatch and save entry keys use these members, not archive-assigned IDs.
    equalInt(int(x.type),int(y.type),where+"type");
    equalInt(x.id,y.id,where+"id");
    // Packed ZenKit output masks these fields or derives them from other members.
    equalInt(int(x.anim_mode),int(y.anim_mode),where+"anim_mode");
    equalInt(x.bias,y.bias,where+"bias");
    equalInt(int(x.sprite_camera_facing_mode),int(y.sprite_camera_facing_mode),where+"sprite_camera_facing_mode");
    equalInt(int(x.dynamic_shadows),int(y.dynamic_shadows),where+"dynamic_shadows");
    equalInt(x.physics_enabled,y.physics_enabled,where+"physics_enabled");
    equalInt(bool(x.visual),bool(y.visual),where+"visual present");
    if(x.visual) equalInt(int(x.visual->type),int(y.visual->type),where+"visual.type");
    if(const auto* trigger=dynamic_cast<const VTrigger*>(&x)) {
      const auto& other=dynamic_cast<const VTrigger&>(y);
      // The engine reads these bools; ZenKit writes only flags/filter_flags.
      equalInt(trigger->start_enabled,other.start_enabled,where+"start_enabled");
      equalInt(trigger->send_untrigger,other.send_untrigger,where+"send_untrigger");
      equalInt(trigger->react_to_on_trigger,other.react_to_on_trigger,where+"react_to_on_trigger");
      equalInt(trigger->react_to_on_touch,other.react_to_on_touch,where+"react_to_on_touch");
      equalInt(trigger->react_to_on_damage,other.react_to_on_damage,where+"react_to_on_damage");
      equalInt(trigger->respond_to_object,other.respond_to_object,where+"respond_to_object");
      equalInt(trigger->respond_to_pc,other.respond_to_pc,where+"respond_to_pc");
      equalInt(trigger->respond_to_npc,other.respond_to_npc,where+"respond_to_npc");
      }
    if(const auto* light=dynamic_cast<const VLight*>(&x))
      compareLight(*light,dynamic_cast<const VLight&>(y),where);
    compareVobTree(x.children,y.children,where+"children");
    }
  }
std::string compareVobs(const std::vector<std::shared_ptr<VirtualObject>>& a,
                        const std::vector<std::shared_ptr<VirtualObject>>& b) {
  compareVobTree(a,b,"vobs");
  const auto serialize=[](const auto& vobs) {
    return ascii([&](WriteArchive& w) {
      require(vobs.size()<=INT32_MAX,"too many root vobs");
      w.write_int("roots",int32_t(vobs.size()));
      for(const auto& v:vobs) {
        require(v!=nullptr,"null root vob");
        save_vob_tree(w,GameVersion::GOTHIC_2,v);
        }
      });
    };
  return textDifference(serialize(a),serialize(b),"vobs");
  }
std::string compareWaynet(const std::shared_ptr<WayNet>& a, const std::shared_ptr<WayNet>& b) {
  equalInt(bool(a),bool(b),"waynet present");
  if(a) {
    equalInt(a->points.size(),b->points.size(),"waynet points count");
    std::unordered_map<const WayPoint*,size_t> aIndices,bIndices;
    for(size_t i=0;i<a->points.size();++i) {
      const auto where="waynet points["+std::to_string(i)+"] ";
      require(a->points[i]!=nullptr && b->points[i]!=nullptr,where+"null waypoint");
      const auto& x=*a->points[i]; const auto& y=*b->points[i];
      aIndices.emplace(&x,i); bIndices.emplace(&y,i);
      equalString(x.name,y.name,where+"name");
      equalInt(x.water_depth,y.water_depth,where+"water_depth");
      equalInt(x.under_water,y.under_water,where+"under_water");
      equalVec(x.position,y.position,where+"position");
      equalVec(x.direction,y.direction,where+"direction");
      equalInt(x.free_point,y.free_point,where+"free_point");
      }
    equalInt(a->edges.size(),b->edges.size(),"waynet edges count");
    for(size_t i=0;i<a->edges.size();++i) {
      const auto where="waynet edges["+std::to_string(i)+"]";
      const auto& x=a->edges[i]; const auto& y=b->edges[i];
      require(aIndices.count(x.first.get()) && aIndices.count(x.second.get()) &&
              bIndices.count(y.first.get()) && bIndices.count(y.second.get()),where+": endpoint outside points");
      equalInt(aIndices.at(x.first.get()),bIndices.at(y.first.get()),where+"[0]");
      equalInt(aIndices.at(x.second.get()),bIndices.at(y.second.get()),where+"[1]");
      }
    }
  auto serialize=[](const auto& p) { return ascii([&](WriteArchive& w){ w.write_object(p,GameVersion::GOTHIC_2); }); };
  return textDifference(serialize(a),serialize(b),"waynet");
  }
template<class A, class Z, class Compare> void importChecked(A& a, Z& z, Compare compare) {
  WorldCopy<> copy; copy.field(a,z);
  Z restored;
  WorldCopy<true> reverse; reverse.field(a,restored);
  const auto difference=compare(z,restored);
  require(difference.empty(),"own type conversion: "+difference);
  }
} // namespace

Data fromWorld(const zenkit::World& world, const WorldChunkSource& source) {
  Data d;
  d.source=source; d.name=world.world_mesh.name; d.bbox=world.world_mesh.bbox;
  // The copier reads the source only; the direction is fixed at compile time.
  auto& w=const_cast<zenkit::World&>(world);
  importChecked(d.vobs,w.world_vobs,compareVobs);
  importChecked(d.waynet,w.way_net,compareWaynet);
  d.bsp.mode=static_cast<asset::BspTreeType>(world.world_bsp_tree.mode);
  d.bsp.leaf_node_indices=world.world_bsp_tree.leaf_node_indices;
  for(const auto& n:world.world_bsp_tree.nodes)
    d.bsp.nodes.push_back({n.plane,n.bbox,n.front_index,n.back_index,n.parent_index,n.lod});
  for(const auto& s:world.world_bsp_tree.sectors)
    d.bsp.sectors.push_back({s.name,s.node_indices,s.portal_polygon_indices});
  d.rooms=asset::roomVolumesFromZenKit(world.world_mesh,world.world_bsp_tree);
  return d;
  }

bool loadZen(const std::vector<std::byte>& bytes, const WorldChunkSource& source, ZenFile& out) {
  out=ZenFile{};
  out.source=source;
  auto stream=Read::from(&bytes);
  auto archive=ReadArchive::from(stream.get());
  require(!archive->is_save_game(),source.world+": save-game archive is not a content .ZEN");
  // BIN_SAFE stores its key table after the objects, before physical EOF.
  size_t objectsEnd=bytes.size();
  if(archive->get_header().format==ArchiveFormat::BINSAFE) {
    const auto start=stream->tell();
    stream->seek(-4,Whence::CUR); // Last u32 of the BIN_SAFE header: key table offset.
    objectsEnd=stream->read_uint();
    stream->seek(static_cast<ssize_t>(start),Whence::BEG);
    require(objectsEnd>=start && objectsEnd<=bytes.size(),"invalid BIN_SAFE key table offset");
    }
  if(source.world=="LIGHTPRESETS.ZEN") {
    out.kind="light_presets";
    const auto count=archive->read_int();
    require(count>=0,"negative light preset count");
    for(int i=0;i<count;++i) {
      ArchiveObject obj;
      require(archive->read_object_begin(obj),"missing light preset object");
      LightPreset preset; preset.load(*archive,GameVersion::GOTHIC_2);
      auto& converted=out.light_presets.emplace_back();
      importChecked(converted,preset,[](const auto& a,const auto& b) {
        compareLight(a,b,"light preset ");
        const auto dump=[](const auto& p) { return ascii([&](WriteArchive& w) { p.save(w,GameVersion::GOTHIC_2); }); };
        return textDifference(dump(a),dump(b),"light preset");
        });
      require(archive->read_object_end(),"unread light preset fields");
      }
    return true;
    }
  if(source.world=="DIALOGCAMS.ZEN") {
    out.kind="cameras";
    std::vector<std::shared_ptr<VirtualObject>> cameras;
    while(stream->tell()<objectsEnd && !stream->eof()) {
      // As in CameraDefinitions: these archives contain consecutive camera presets.
      ArchiveObject obj;
      if(!archive->read_object_begin(obj)) {
        require(stream->eof(),"expected camera object");
        break;
        }
      require(obj.class_name=="zCCSCamera:zCVob","unknown camera class "+obj.class_name);
      auto camera=std::make_shared<VCutsceneCamera>();
      camera->type=VirtualObjectType::zCCSCamera; camera->id=obj.index;
      camera->load(*archive,GameVersion::GOTHIC_2);
      require(archive->read_object_end(),"unread camera fields");
      require(stream->tell()<=objectsEnd,"camera overlaps BIN_SAFE key table");
      cameras.push_back(camera);
      }
    importChecked(out.vobs,cameras,compareVobs);
    return true;
    }
  World world;
  stream=Read::from(&bytes);
  world.load(stream.get(),GameVersion::GOTHIC_2);
  if(!world.world_mesh.vertices.empty()) return false;
  out.kind="world";
  importChecked(out.vobs,world.world_vobs,compareVobs);
  importChecked(out.waynet,world.way_net,compareWaynet);
  return true;
  }

void loadCutsceneLibrary(const std::vector<std::byte>& bytes, const WorldChunkSource& source, ZenFile& out) {
  out=ZenFile{};
  out.source=source;
  out.kind="cutscene_library";
  // The roots as written to a library, as one ZenKit ASCII archive (graph references included).
  const auto libraryText=[](const std::vector<std::shared_ptr<CutsceneBlock>>& blocks) {
    auto library=std::make_shared<CutsceneLibrary>();
    library->blocks=blocks;
    return ascii([&](WriteArchive& w) { w.write_object(library,GameVersion::GOTHIC_2); });
    };
  // CutsceneLibrary::load inside ReadArchive::read_object, without the sort.
  std::vector<std::shared_ptr<CutsceneBlock>> roots;
  {
  auto stream=Read::from(&bytes);
  auto archive=ReadArchive::from(stream.get());
  require(archive!=nullptr,source.world+": not a ZenGin archive");
  ArchiveObject obj;
  require(archive->read_object_begin(obj),source.world+": expected a library object");
  require(obj.class_name=="zCCSLib",source.world+": root is "+obj.class_name+", not zCCSLib");
  const auto count=archive->read_int(); // NumOfItems
  for(int32_t i=0;i<count;++i) {
    auto block=archive->read_object<CutsceneBlock>(GameVersion::GOTHIC_2);
    require(block!=nullptr,source.world+": null library root "+std::to_string(i));
    roots.push_back(std::move(block));
    }
  if(!archive->read_object_end())
    archive->skip_object(true);
  }
  importChecked(out.blocks,roots,[&](const auto& a,const auto& b) {
    return textDifference(libraryText(a),libraryText(b),source.world+" library");
    });
  // ZenKit's own reader: its sorted blocks must be the same std::sort of these roots.
  auto stream=Read::from(&bytes);
  auto archive=ReadArchive::from(stream.get());
  require(archive!=nullptr,source.world+": not a ZenGin archive");
  const auto library=archive->read_object<CutsceneLibrary>(GameVersion::GOTHIC_2);
  require(library!=nullptr,source.world+": ZenKit reads no cutscene library");
  auto sorted=roots;
  std::sort(sorted.begin(),sorted.end(),[](auto const& a, auto const& b) { return a->name < b->name; });
  const auto difference=textDifference(libraryText(library->blocks),libraryText(sorted),source.world+" sorted library");
  require(difference.empty(),difference);
  }

void selfTest(const fs::path& dir) {
  World world;
  auto& mesh=world.world_mesh;
  mesh.name="ROOM_\xE4";
  mesh.bbox.min={-400,-400,-400}; mesh.bbox.max={400,400,400};
  mesh.vertices={{-100,-100,-100},{100,-100,-100},{100,100,-100},{-100,100,-100},
                 {-100,-100,100},{100,-100,100},{100,100,100},{-100,100,100}};
  const uint32_t faces[][4]={{0,1,2,3},{4,5,6,7},{0,1,5,4},{3,2,6,7},{0,3,7,4},{1,2,6,5}};
  for(const auto& face:faces) {
    Polygon p{}; p.index_offset=mesh.polygon_vertex_indices.size(); p.index_count=4;
    p.flags.is_sector=mesh.geometry.size()!=5;
    mesh.geometry.push_back(p);
    mesh.polygon_vertex_indices.insert(mesh.polygon_vertex_indices.end(),std::begin(face),std::end(face));
    }
  auto& bsp=world.world_bsp_tree;
  bsp.mode=BspTreeType::OUTDOOR;
  bsp.nodes.resize(3);
  bsp.nodes[0].front_index=1; bsp.nodes[0].back_index=2;
  bsp.nodes[1].parent_index=bsp.nodes[2].parent_index=0;
  bsp.nodes[1].polygon_index=0; bsp.nodes[1].polygon_count=5;
  bsp.nodes[0].plane={1.25f,-0.f,0.125f,42.f}; bsp.nodes[0].lod=3;
  for(auto& n:bsp.nodes) n.bbox=mesh.bbox;
  bsp.polygon_indices={0,1,2,3,4}; bsp.leaf_node_indices={1,2};
  bsp.sectors={{"ROOM_\xE4",{0},{5}},{"EMPTY",{1},{}}};

  for(int i=0;i<44;++i) {
    auto object=makeOriginalObject(ObjectType(i));
    auto v=std::dynamic_pointer_cast<VirtualObject>(object);
    require(v!=nullptr,"self-test missing vob class "+std::to_string(i));
    v->type=VirtualObjectType(i); v->id=uint32_t(i+100);
    v->vob_name="VOB_"+std::to_string(i)+"_\xE4\xFF";
    v->position={-0.f,0.1f,12345.678f}; v->rotation={{1,2,3},{4,5,6},{7,8,9}};
    v->anim_mode=AnimationType(3);
    world.world_vobs.push_back(v);
    }
  auto decal=std::make_shared<VisualDecal>(); decal->type=VisualType::DECAL; decal->name="DECAL_\xFF.TGA";
  decal->dimension={33.3f,45.6f}; decal->offset={-0.f,0.1f}; decal->alpha_weight=127;
  decal->alpha_func=AlphaFunction::BLEND; decal->two_sided=true; decal->ignore_daylight=true;
  decal->texture_anim_fps=0.123456789f;
  world.world_vobs[0]->visual=world.world_vobs[1]->visual=decal;
  world.world_vobs[0]->visual_decal=*decal;
  world.world_vobs[0]->children.push_back(std::make_shared<VItem>());
  auto camera=std::dynamic_pointer_cast<VCutsceneCamera>(world.world_vobs[16]);
  auto frame=std::dynamic_pointer_cast<VCameraTrajectoryFrame>(world.world_vobs[17]);
  camera->trajectory_frames={frame}; camera->target_frames={frame}; camera->position_count=camera->target_count=1;
  frame->original_pose[1][2]=0.123456789f;
  auto npc=std::dynamic_pointer_cast<VNpc>(world.world_vobs[3]);
  auto item=std::dynamic_pointer_cast<VItem>(world.world_vobs[2]);
  npc->items={item}; npc->talents.push_back(std::make_shared<VNpc::Talent>());
  npc->talents[0]->talent=7; npc->talents[0]->value=9;
  npc->slots.push_back(std::make_unique<VNpc::Slot>()); npc->slots[0]->item=item; npc->slots[0]->used=true;
  npc->news.push_back(std::make_unique<VNpc::News>()); npc->news[0]->news_id=VNpc::NewsId::THEFT;
  auto ai=std::make_shared<AiMove>(); ai->vob=item; ai->owner=npc; item->ai=ai;
  std::dynamic_pointer_cast<VContainer>(world.world_vobs[28])->contents="ITMI_GOLD:12";
  auto mover=std::dynamic_pointer_cast<VMover>(world.world_vobs[35]);
  mover->flags=7; mover->filter_flags=0x3f;
  mover->start_enabled=true; mover->respond_to_pc=true; mover->send_untrigger=true;
  auto light=std::dynamic_pointer_cast<VLight>(world.world_vobs[10]);
  light->is_static=true; light->on=true; light->can_move=true;
  light->range_animation_scale={0.25f,1.5f}; light->color_animation_list={{3,4,5,6}};
  mover->keyframes.push_back({{1,2,3},{0.1f,0.2f,0.3f,0.4f}});

  world.way_net=std::make_shared<WayNet>();
  for(int i=0;i<3;++i) {
    auto point=std::make_shared<WayPoint>(); point->name="WP_"+std::to_string(i)+"_\xFC";
    point->position={float(i),-0.f,0.1f}; point->direction={0,1,0}; point->free_point=i==1;
    world.way_net->points.push_back(point);
    }
  world.way_net->edges={{world.way_net->points[2],world.way_net->points[0]},
                       {world.way_net->points[1],world.way_net->points[2]}};
  const auto original=fromWorld(world,{"TEST.ZEN",1234,0xDEADBEEFu});
  require(original.rooms[0].triangles.size()==12 && original.rooms[1].triangles.empty(),"room polygon fan or portal cap");
  require(isIndoor(static_cast<asset::BspTreeType>(bsp.mode),original.rooms,{0,0,0}) && !isIndoor(static_cast<asset::BspTreeType>(bsp.mode),original.rooms,{200,0,0}),"closed room containment");
  write(original,dir);
  const auto back=read(dir);
  const auto difference=compare(original,back); require(difference.empty(),"self-test round trip: "+difference);
  require(back.vobs[0]->anim_mode==asset::AnimationType(3),"unnamed enum must retain raw value 3");
  equalFloat(original.vobs[0]->position.x,back.vobs[0]->position.x,"negative zero");
  require(back.vobs[0]->rotation==original.vobs[0]->rotation,"matrix rows");
  require(back.vobs[0]->visual==back.vobs[1]->visual,"shared visual identity");
  const auto backNpc=std::dynamic_pointer_cast<asset::VNpc>(back.vobs[3]);
  const auto backAi=std::dynamic_pointer_cast<asset::AiMove>(back.vobs[2]->ai);
  require(backNpc->items[0]==back.vobs[2] && backNpc->slots[0]->item==back.vobs[2],"shared inventory identity");
  require(backAi->vob.lock()==back.vobs[2] && backAi->owner.lock()==backNpc,"weak back references");
  const auto backCamera=std::dynamic_pointer_cast<asset::VCutsceneCamera>(back.vobs[16]);
  require(backCamera->trajectory_frames[0]==back.vobs[17] &&
          backCamera->target_frames[0]==back.vobs[17],"shared camera frame identity");
  require(back.waynet->points[0]->name==original.waynet->points[0]->name,"waypoint order");
  for(const auto* kind:{"world","cameras","light_presets"}) {
    ZenFile zen; zen.source={"SYNTHETIC.ZEN",10,20}; zen.kind=kind;
    if(zen.kind=="light_presets") {
      zen.light_presets.emplace_back(); auto& light=zen.light_presets.back();
      light.preset="LIGHT_\xE4"; light.range_animation_scale={0.1f,1.25f};
      light.color_animation_list={{1,2,3,255}}; light.light_type=asset::LightType(17);
      } else { zen.vobs=original.vobs; zen.waynet=original.waynet; }
    const auto file=dir/(zen.kind+".json"); writeZen(zen,file);
    const auto restored=readZen(file); const auto diff=compareZen(zen,restored);
    require(diff.empty(),"self-test ZEN: "+diff);
    if(zen.kind=="light_presets") require(restored.light_presets[0].light_type==asset::LightType(17),"unnamed light enum");
    }
  // The installed archives may not contain camera/light presets. Exercise their
  // original .ZEN reader with ZenKit-produced archives as well as our JSON files.
  for(const auto format:{ArchiveFormat::BINARY,ArchiveFormat::BINSAFE}) for(bool lights:{false,true}) {
    std::vector<std::byte> bytes;
    auto stream=Write::to(&bytes);
    auto archive=WriteArchive::to(stream.get(),format);
    if(lights) {
      archive->write_int("count",1);
      archive->write_object_begin("preset","zCLightPreset",0);
      LightPreset preset; preset.preset="TEST_LIGHT"; preset.is_static=true; preset.range=12.5f;
      preset.save(*archive,GameVersion::GOTHIC_2);
      archive->write_object_end();
      } else {
      camera->visual=decal;
      archive->write_object(camera,GameVersion::GOTHIC_2);
      auto second=std::make_shared<VCutsceneCamera>(*camera);
      archive->write_object(second,GameVersion::GOTHIC_2);
      }
    archive->write_header();
    ZenFile zen;
    const WorldChunkSource source{lights ? "LIGHTPRESETS.ZEN" : "DIALOGCAMS.ZEN",bytes.size(),
                                  worldChunkCrc32(bytes.data(),bytes.size())};
    require(loadZen(bytes,source,zen),"synthetic presets must be meshless");
    if(!lights) {
      require(zen.vobs.size()==2 && zen.vobs[0]->visual==zen.vobs[1]->visual,"visual shared across camera presets");
      const auto a=std::dynamic_pointer_cast<asset::VCutsceneCamera>(zen.vobs[0]);
      const auto b=std::dynamic_pointer_cast<asset::VCutsceneCamera>(zen.vobs[1]);
      require(a->trajectory_frames[0]==b->trajectory_frames[0],"frame shared across camera presets");
      }
    const auto file=dir/(source.world+".json"); writeZen(zen,file);
    const auto diff=compareZen(zen,readZen(file));
    require(diff.empty(),"preset archive round trip: "+diff);
    }
  // A named enum can also be supplied as its raw number by an editor.
  const auto sectorBytes=gothicglb::readFile(dir/"sectors.json");
  std::string sectors(sectorBytes.begin(),sectorBytes.end());
  const std::string named="\"mode\":\"OUTDOOR\"";
  const auto modeAt=sectors.find(named);
  require(modeAt!=std::string::npos,"self-test missing BSP mode");
  sectors.replace(modeAt,named.size(),"\"mode\":1");
  gothicglb::writeFile(dir/"sectors.json",sectors.data(),sectors.size());
  require(read(dir).bsp.mode==asset::BspTreeType::OUTDOOR,"numeric known enum");
  }

} // namespace WorldData
