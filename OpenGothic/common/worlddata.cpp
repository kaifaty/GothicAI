#include "worlddata.h"
#include "gothicglb.h"

#include <rapidjson/error/en.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

namespace WorldData {
namespace {
using namespace asset;
using zenkit::Vec2;
using zenkit::Vec3;
using zenkit::Quat;
using zenkit::Color;
using zenkit::Mat3;
using zenkit::Mat4;
using zenkit::AxisAlignedBoundingBox;
using Json = rapidjson::Value;
using gothicglb::jsonString;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("world data: "+reason);
  }
void require(bool ok, const std::string& reason) { if(!ok) fail(reason); }
const Json& member(const Json& obj, const char* name) {
  require(obj.IsObject() && obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }
void array(const Json& v) { require(v.IsArray(),"expected array"); }
void array(const Json& v, size_t count) {
  array(v);
  require(v.Size()==count,"expected array of "+std::to_string(count)+" elements");
  }
std::string floatJson(float v) {
  require(std::isfinite(v),"non-finite float");
  // JSON integer -0 would lose its sign in RapidJSON.
  return v==0 && std::signbit(v) ? "-0.0" : gothicglb::jsonFloat(v);
  }

template<class E> const std::vector<std::pair<E,const char*>>& enumNames();

template<> const std::vector<std::pair<ShadowType,const char*>>& enumNames<ShadowType>() {
  static const std::vector<std::pair<ShadowType,const char*>> names = {
    {ShadowType::NONE,"NONE"},
    {ShadowType::BLOB,"BLOB"},
    };
  return names;
  }

template<> const std::vector<std::pair<VisualType,const char*>>& enumNames<VisualType>() {
  static const std::vector<std::pair<VisualType,const char*>> names = {
    {VisualType::DECAL,"DECAL"},
    {VisualType::MESH,"MESH"},
    {VisualType::MULTI_RESOLUTION_MESH,"MULTI_RESOLUTION_MESH"},
    {VisualType::PARTICLE_EFFECT,"PARTICLE_EFFECT"},
    {VisualType::AI_CAMERA,"AI_CAMERA"},
    {VisualType::MODEL,"MODEL"},
    {VisualType::MORPH_MESH,"MORPH_MESH"},
    {VisualType::UNKNOWN,"UNKNOWN"},
    };
  return names;
  }

template<> const std::vector<std::pair<SpriteAlignment,const char*>>& enumNames<SpriteAlignment>() {
  static const std::vector<std::pair<SpriteAlignment,const char*>> names = {
    {SpriteAlignment::NONE,"NONE"},
    {SpriteAlignment::YAW,"YAW"},
    {SpriteAlignment::FULL,"FULL"},
    };
  return names;
  }

template<> const std::vector<std::pair<AnimationType,const char*>>& enumNames<AnimationType>() {
  static const std::vector<std::pair<AnimationType,const char*>> names = {
    {AnimationType::NONE,"NONE"},
    {AnimationType::WIND,"WIND"},
    {AnimationType::WIND_ALT,"WIND_ALT"},
    };
  return names;
  }

template<> const std::vector<std::pair<VirtualObjectType,const char*>>& enumNames<VirtualObjectType>() {
  static const std::vector<std::pair<VirtualObjectType,const char*>> names = {
    {VirtualObjectType::UNKNOWN,"UNKNOWN"},
    {VirtualObjectType::zCVob,"zCVob"},
    {VirtualObjectType::zCVobLevelCompo,"zCVobLevelCompo"},
    {VirtualObjectType::oCItem,"oCItem"},
    {VirtualObjectType::oCNpc,"oCNpc"},
    {VirtualObjectType::zCMoverController,"zCMoverController"},
    {VirtualObjectType::zCVobScreenFX,"zCVobScreenFX"},
    {VirtualObjectType::zCVobStair,"zCVobStair"},
    {VirtualObjectType::zCPFXController,"zCPFXController"},
    {VirtualObjectType::zCVobAnimate,"zCVobAnimate"},
    {VirtualObjectType::zCVobLensFlare,"zCVobLensFlare"},
    {VirtualObjectType::zCVobLight,"zCVobLight"},
    {VirtualObjectType::zCVobSpot,"zCVobSpot"},
    {VirtualObjectType::zCVobStartpoint,"zCVobStartpoint"},
    {VirtualObjectType::zCMessageFilter,"zCMessageFilter"},
    {VirtualObjectType::zCCodeMaster,"zCCodeMaster"},
    {VirtualObjectType::zCTriggerWorldStart,"zCTriggerWorldStart"},
    {VirtualObjectType::zCCSCamera,"zCCSCamera"},
    {VirtualObjectType::zCCamTrj_KeyFrame,"zCCamTrj_KeyFrame"},
    {VirtualObjectType::oCTouchDamage,"oCTouchDamage"},
    {VirtualObjectType::zCTriggerUntouch,"zCTriggerUntouch"},
    {VirtualObjectType::zCEarthquake,"zCEarthquake"},
    {VirtualObjectType::oCMOB,"oCMOB"},
    {VirtualObjectType::oCMobInter,"oCMobInter"},
    {VirtualObjectType::oCMobBed,"oCMobBed"},
    {VirtualObjectType::oCMobFire,"oCMobFire"},
    {VirtualObjectType::oCMobLadder,"oCMobLadder"},
    {VirtualObjectType::oCMobSwitch,"oCMobSwitch"},
    {VirtualObjectType::oCMobWheel,"oCMobWheel"},
    {VirtualObjectType::oCMobContainer,"oCMobContainer"},
    {VirtualObjectType::oCMobDoor,"oCMobDoor"},
    {VirtualObjectType::zCTrigger,"zCTrigger"},
    {VirtualObjectType::zCTriggerList,"zCTriggerList"},
    {VirtualObjectType::oCTriggerScript,"oCTriggerScript"},
    {VirtualObjectType::oCTriggerChangeLevel,"oCTriggerChangeLevel"},
    {VirtualObjectType::oCCSTrigger,"oCCSTrigger"},
    {VirtualObjectType::zCMover,"zCMover"},
    {VirtualObjectType::zCVobSound,"zCVobSound"},
    {VirtualObjectType::zCVobSoundDaytime,"zCVobSoundDaytime"},
    {VirtualObjectType::oCZoneMusic,"oCZoneMusic"},
    {VirtualObjectType::oCZoneMusicDefault,"oCZoneMusicDefault"},
    {VirtualObjectType::zCZoneZFog,"zCZoneZFog"},
    {VirtualObjectType::zCZoneZFogDefault,"zCZoneZFogDefault"},
    {VirtualObjectType::zCZoneVobFarPlane,"zCZoneVobFarPlane"},
    {VirtualObjectType::zCZoneVobFarPlaneDefault,"zCZoneVobFarPlaneDefault"},
    };
  return names;
  }

template<> const std::vector<std::pair<CameraMotion,const char*>>& enumNames<CameraMotion>() {
  static const std::vector<std::pair<CameraMotion,const char*>> names = {
    {CameraMotion::UNDEFINED,"UNDEFINED"},
    {CameraMotion::SMOOTH,"SMOOTH"},
    {CameraMotion::LINEAR,"LINEAR"},
    {CameraMotion::STEP,"STEP"},
    {CameraMotion::SLOW,"SLOW"},
    {CameraMotion::FAST,"FAST"},
    {CameraMotion::CUSTOM,"CUSTOM"},
    };
  return names;
  }

template<> const std::vector<std::pair<CameraCoordinateReference,const char*>>& enumNames<CameraCoordinateReference>() {
  static const std::vector<std::pair<CameraCoordinateReference,const char*>> names = {
    {CameraCoordinateReference::WORLD,"WORLD"},
    {CameraCoordinateReference::OBJECT,"OBJECT"},
    };
  return names;
  }

template<> const std::vector<std::pair<CameraLerpType,const char*>>& enumNames<CameraLerpType>() {
  static const std::vector<std::pair<CameraLerpType,const char*>> names = {
    {CameraLerpType::UNDEFINED,"UNDEFINED"},
    {CameraLerpType::PATH,"PATH"},
    {CameraLerpType::PATH_IGNORE_ROLL,"PATH_IGNORE_ROLL"},
    {CameraLerpType::PATH_ROTATION_SAMPLES,"PATH_ROTATION_SAMPLES"},
    };
  return names;
  }

template<> const std::vector<std::pair<CameraLoop,const char*>>& enumNames<CameraLoop>() {
  static const std::vector<std::pair<CameraLoop,const char*>> names = {
    {CameraLoop::NONE,"NONE"},
    {CameraLoop::RESTART,"RESTART"},
    {CameraLoop::PINGPONG,"PINGPONG"},
    };
  return names;
  }

template<> const std::vector<std::pair<LightType,const char*>>& enumNames<LightType>() {
  static const std::vector<std::pair<LightType,const char*>> names = {
    {LightType::POINT,"POINT"},
    {LightType::SPOT,"SPOT"},
    {LightType::RESERVED0,"RESERVED0"},
    {LightType::RESERVED1,"RESERVED1"},
    };
  return names;
  }

template<> const std::vector<std::pair<LightQuality,const char*>>& enumNames<LightQuality>() {
  static const std::vector<std::pair<LightQuality,const char*>> names = {
    {LightQuality::HIGH,"HIGH"},
    {LightQuality::MEDIUM,"MEDIUM"},
    {LightQuality::LOW,"LOW"},
    };
  return names;
  }

template<> const std::vector<std::pair<MessageFilterAction,const char*>>& enumNames<MessageFilterAction>() {
  static const std::vector<std::pair<MessageFilterAction,const char*>> names = {
    {MessageFilterAction::NONE,"NONE"},
    {MessageFilterAction::TRIGGER,"TRIGGER"},
    {MessageFilterAction::UNTRIGGER,"UNTRIGGER"},
    {MessageFilterAction::ENABLE,"ENABLE"},
    {MessageFilterAction::DISABLE,"DISABLE"},
    {MessageFilterAction::TOGGLE,"TOGGLE"},
    };
  return names;
  }

template<> const std::vector<std::pair<MoverMessageType,const char*>>& enumNames<MoverMessageType>() {
  static const std::vector<std::pair<MoverMessageType,const char*>> names = {
    {MoverMessageType::FIXED_DIRECT,"FIXED_DIRECT"},
    {MoverMessageType::FIXED_ORDER,"FIXED_ORDER"},
    {MoverMessageType::NEXT,"NEXT"},
    {MoverMessageType::PREVIOUS,"PREVIOUS"},
    };
  return names;
  }

template<> const std::vector<std::pair<TouchCollisionType,const char*>>& enumNames<TouchCollisionType>() {
  static const std::vector<std::pair<TouchCollisionType,const char*>> names = {
    {TouchCollisionType::NONE,"NONE"},
    {TouchCollisionType::BOX,"BOX"},
    {TouchCollisionType::POINT,"POINT"},
    };
  return names;
  }

template<> const std::vector<std::pair<VNpc::NewsId,const char*>>& enumNames<VNpc::NewsId>() {
  static const std::vector<std::pair<VNpc::NewsId,const char*>> names = {
    {VNpc::NewsId::MURDER,"MURDER"},
    {VNpc::NewsId::ATTACK,"ATTACK"},
    {VNpc::NewsId::THEFT,"THEFT"},
    {VNpc::NewsId::DEFEAT,"DEFEAT"},
    {VNpc::NewsId::NERVE,"NERVE"},
    {VNpc::NewsId::INTERFERE,"INTERFERE"},
    {VNpc::NewsId::HAS_DEFEATED,"HAS_DEFEATED"},
    };
  return names;
  }

template<> const std::vector<std::pair<VNpc::NewsSpread,const char*>>& enumNames<VNpc::NewsSpread>() {
  static const std::vector<std::pair<VNpc::NewsSpread,const char*>> names = {
    {VNpc::NewsSpread::DONT_SPREAD,"DONT_SPREAD"},
    {VNpc::NewsSpread::FRIENDLY_TOWARDS_VICTIM,"FRIENDLY_TOWARDS_VICTIM"},
    {VNpc::NewsSpread::FRIENDLY_TOWARDS_WITNESS,"FRIENDLY_TOWARDS_WITNESS"},
    {VNpc::NewsSpread::FRIENDLY_TOWARDS_OFFENDER,"FRIENDLY_TOWARDS_OFFENDER"},
    {VNpc::NewsSpread::SAME_GUILD_VICTIM,"SAME_GUILD_VICTIM"},
    };
  return names;
  }

template<> const std::vector<std::pair<SoundMaterialType,const char*>>& enumNames<SoundMaterialType>() {
  static const std::vector<std::pair<SoundMaterialType,const char*>> names = {
    {SoundMaterialType::WOOD,"WOOD"},
    {SoundMaterialType::STONE,"STONE"},
    {SoundMaterialType::METAL,"METAL"},
    {SoundMaterialType::LEATHER,"LEATHER"},
    {SoundMaterialType::CLAY,"CLAY"},
    {SoundMaterialType::GLASS,"GLASS"},
    };
  return names;
  }

template<> const std::vector<std::pair<SoundMode,const char*>>& enumNames<SoundMode>() {
  static const std::vector<std::pair<SoundMode,const char*>> names = {
    {SoundMode::LOOP,"LOOP"},
    {SoundMode::ONCE,"ONCE"},
    {SoundMode::RANDOM,"RANDOM"},
    };
  return names;
  }

template<> const std::vector<std::pair<SoundTriggerVolumeType,const char*>>& enumNames<SoundTriggerVolumeType>() {
  static const std::vector<std::pair<SoundTriggerVolumeType,const char*>> names = {
    {SoundTriggerVolumeType::SPHERICAL,"SPHERICAL"},
    {SoundTriggerVolumeType::ELLIPSOIDAL,"ELLIPSOIDAL"},
    };
  return names;
  }

template<> const std::vector<std::pair<MoverBehavior,const char*>>& enumNames<MoverBehavior>() {
  static const std::vector<std::pair<MoverBehavior,const char*>> names = {
    {MoverBehavior::TOGGLE,"TOGGLE"},
    {MoverBehavior::TRIGGER_CONTROL,"TRIGGER_CONTROL"},
    {MoverBehavior::OPEN_TIME,"OPEN_TIME"},
    {MoverBehavior::LOOP,"LOOP"},
    {MoverBehavior::SINGLE_KEYS,"SINGLE_KEYS"},
    };
  return names;
  }

template<> const std::vector<std::pair<MoverLerpType,const char*>>& enumNames<MoverLerpType>() {
  static const std::vector<std::pair<MoverLerpType,const char*>> names = {
    {MoverLerpType::LINEAR,"LINEAR"},
    {MoverLerpType::CURVE,"CURVE"},
    };
  return names;
  }

template<> const std::vector<std::pair<MoverSpeedType,const char*>>& enumNames<MoverSpeedType>() {
  static const std::vector<std::pair<MoverSpeedType,const char*>> names = {
    {MoverSpeedType::CONSTANT,"CONSTANT"},
    {MoverSpeedType::SLOW_START_END,"SLOW_START_END"},
    {MoverSpeedType::SLOW_START,"SLOW_START"},
    {MoverSpeedType::SLOW_END,"SLOW_END"},
    {MoverSpeedType::SEGMENT_SLOW_START_END,"SEGMENT_SLOW_START_END"},
    {MoverSpeedType::SEGMENT_SLOW_START,"SEGMENT_SLOW_START"},
    {MoverSpeedType::SEGMENT_SLOW_END,"SEGMENT_SLOW_END"},
    };
  return names;
  }

template<> const std::vector<std::pair<TriggerBatchMode,const char*>>& enumNames<TriggerBatchMode>() {
  static const std::vector<std::pair<TriggerBatchMode,const char*>> names = {
    {TriggerBatchMode::ALL,"ALL"},
    {TriggerBatchMode::NEXT,"NEXT"},
    {TriggerBatchMode::RANDOM,"RANDOM"},
    };
  return names;
  }

template<> const std::vector<std::pair<AlphaFunction,const char*>>& enumNames<AlphaFunction>() {
  static const std::vector<std::pair<AlphaFunction,const char*>> names = {
    {AlphaFunction::DEFAULT,"DEFAULT"},
    {AlphaFunction::NONE,"NONE"},
    {AlphaFunction::BLEND,"BLEND"},
    {AlphaFunction::ADD,"ADD"},
    {AlphaFunction::SUBTRACT,"SUBTRACT"},
    {AlphaFunction::MULTIPLY,"MULTIPLY"},
    {AlphaFunction::MULTIPLY_ALT,"MULTIPLY_ALT"},
    };
  return names;
  }

template<> const std::vector<std::pair<ObjectType,const char*>>& enumNames<ObjectType>() {
  static const std::vector<std::pair<ObjectType,const char*>> names = {
    {ObjectType::zCVob,"zCVob"},
    {ObjectType::zCVobLevelCompo,"zCVobLevelCompo"},
    {ObjectType::oCItem,"oCItem"},
    {ObjectType::oCNpc,"oCNpc"},
    {ObjectType::zCMoverController,"zCMoverController"},
    {ObjectType::zCVobScreenFX,"zCVobScreenFX"},
    {ObjectType::zCVobStair,"zCVobStair"},
    {ObjectType::zCPFXController,"zCPFXController"},
    {ObjectType::zCVobAnimate,"zCVobAnimate"},
    {ObjectType::zCVobLensFlare,"zCVobLensFlare"},
    {ObjectType::zCVobLight,"zCVobLight"},
    {ObjectType::zCVobSpot,"zCVobSpot"},
    {ObjectType::zCVobStartpoint,"zCVobStartpoint"},
    {ObjectType::zCMessageFilter,"zCMessageFilter"},
    {ObjectType::zCCodeMaster,"zCCodeMaster"},
    {ObjectType::zCTriggerWorldStart,"zCTriggerWorldStart"},
    {ObjectType::zCCSCamera,"zCCSCamera"},
    {ObjectType::zCCamTrj_KeyFrame,"zCCamTrj_KeyFrame"},
    {ObjectType::oCTouchDamage,"oCTouchDamage"},
    {ObjectType::zCTriggerUntouch,"zCTriggerUntouch"},
    {ObjectType::zCEarthquake,"zCEarthquake"},
    {ObjectType::oCMOB,"oCMOB"},
    {ObjectType::oCMobInter,"oCMobInter"},
    {ObjectType::oCMobBed,"oCMobBed"},
    {ObjectType::oCMobFire,"oCMobFire"},
    {ObjectType::oCMobLadder,"oCMobLadder"},
    {ObjectType::oCMobSwitch,"oCMobSwitch"},
    {ObjectType::oCMobWheel,"oCMobWheel"},
    {ObjectType::oCMobContainer,"oCMobContainer"},
    {ObjectType::oCMobDoor,"oCMobDoor"},
    {ObjectType::zCTrigger,"zCTrigger"},
    {ObjectType::zCTriggerList,"zCTriggerList"},
    {ObjectType::oCTriggerScript,"oCTriggerScript"},
    {ObjectType::oCTriggerChangeLevel,"oCTriggerChangeLevel"},
    {ObjectType::oCCSTrigger,"oCCSTrigger"},
    {ObjectType::zCMover,"zCMover"},
    {ObjectType::zCVobSound,"zCVobSound"},
    {ObjectType::zCVobSoundDaytime,"zCVobSoundDaytime"},
    {ObjectType::oCZoneMusic,"oCZoneMusic"},
    {ObjectType::oCZoneMusicDefault,"oCZoneMusicDefault"},
    {ObjectType::zCZoneZFog,"zCZoneZFog"},
    {ObjectType::zCZoneZFogDefault,"zCZoneZFogDefault"},
    {ObjectType::zCZoneVobFarPlane,"zCZoneVobFarPlane"},
    {ObjectType::zCZoneVobFarPlaneDefault,"zCZoneVobFarPlaneDefault"},
    {ObjectType::ignored,"ignored"},
    {ObjectType::unknown,"unknown"},
    {ObjectType::oCNpcTalent,"oCNpcTalent"},
    {ObjectType::zCEventManager,"zCEventManager"},
    {ObjectType::zCDecal,"zCDecal"},
    {ObjectType::zCMesh,"zCMesh"},
    {ObjectType::zCProgMeshProto,"zCProgMeshProto"},
    {ObjectType::zCParticleFX,"zCParticleFX"},
    {ObjectType::zCAICamera,"zCAICamera"},
    {ObjectType::zCModel,"zCModel"},
    {ObjectType::zCMorphMesh,"zCMorphMesh"},
    {ObjectType::oCAIHuman,"oCAIHuman"},
    {ObjectType::oCAIVobMove,"oCAIVobMove"},
    {ObjectType::oCCSPlayer,"oCCSPlayer"},
    {ObjectType::zCSkyControler_Outdoor,"zCSkyControler_Outdoor"},
    {ObjectType::oCWorld,"oCWorld"},
    {ObjectType::zCWayNet,"zCWayNet"},
    {ObjectType::zCWaypoint,"zCWaypoint"},
    {ObjectType::zCMaterial,"zCMaterial"},
    {ObjectType::oCSavegameInfo,"oCSavegameInfo"},
    {ObjectType::oCCSManager,"oCCSManager"},
    {ObjectType::zCCSPoolItem,"zCCSPoolItem"},
    {ObjectType::zCCSBlock,"zCCSBlock"},
    {ObjectType::zCCutscene,"zCCutscene"},
    {ObjectType::zCCSCutsceneContext,"zCCSCutsceneContext"},
    {ObjectType::oCMsgConversation,"oCMsgConversation"},
    {ObjectType::zCCSAtomicBlock,"zCCSAtomicBlock"},
    {ObjectType::zCCSLib,"zCCSLib"},
    {ObjectType::zCCSProps,"zCCSProps"},
    };
  return names;
  }

template<> const std::vector<std::pair<BspTreeType,const char*>>& enumNames<BspTreeType>() {
  static const std::vector<std::pair<BspTreeType,const char*>> names = {
    {BspTreeType::INDOOR,"INDOOR"},
    {BspTreeType::OUTDOOR,"OUTDOOR"},
    };
  return names;
  }

// Explicit C++ member names: the JSON schema does not depend on ZenKit archive keys.
template<class IO> void fields(IO& io, AxisAlignedBoundingBox& v) {
  io("min",v.min);
  io("max",v.max);
  }

template<class IO> void fields(IO& io, MoverKeyframe& v) {
  io("position",v.position);
  io("rotation",v.rotation);
  }

template<class IO> void fields(IO& io, RigidBody& v) {
  io("vel",v.vel);
  io("mode",v.mode);
  io("gravity_enabled",v.gravity_enabled);
  io("gravity_scale",v.gravity_scale);
  io("slide_direction",v.slide_direction);
  }

template<class IO> void fields(IO& io, Visual& v) {
  io("type",v.type);
  io("name",v.name);
  }

template<class IO> void fields(IO& io, VisualDecal& v) {
  fields(io,static_cast<Visual&>(v));
  io("dimension",v.dimension);
  io("offset",v.offset);
  io("two_sided",v.two_sided);
  io("alpha_func",v.alpha_func);
  io("texture_anim_fps",v.texture_anim_fps);
  io("alpha_weight",v.alpha_weight);
  io("ignore_daylight",v.ignore_daylight);
  }

template<class IO> void fields(IO& io, VirtualObject& v) {
  io("type",v.type);
  io("id",v.id);
  io("bbox",v.bbox);
  io("position",v.position);
  io("rotation",v.rotation);
  io("show_visual",v.show_visual);
  io("sprite_camera_facing_mode",v.sprite_camera_facing_mode);
  io("cd_static",v.cd_static);
  io("cd_dynamic",v.cd_dynamic);
  io("vob_static",v.vob_static);
  io("dynamic_shadows",v.dynamic_shadows);
  io("physics_enabled",v.physics_enabled);
  io("anim_mode",v.anim_mode);
  io("bias",v.bias);
  io("ambient",v.ambient);
  io("anim_strength",v.anim_strength);
  io("far_clip_scale",v.far_clip_scale);
  io("preset_name",v.preset_name);
  io("vob_name",v.vob_name);
  io("visual_name",v.visual_name);
  io("associated_visual_type",v.associated_visual_type);
  io("visual_decal",v.visual_decal);
  io("visual",v.visual);
  io("ai",v.ai);
  io("event_manager",v.event_manager);
  io("sleep_mode",v.sleep_mode);
  io("next_on_timer",v.next_on_timer);
  io("rigid_body",v.rigid_body);
  io("children",v.children);
  }

template<class IO> void fields(IO& io, EventManager& v) {
  io("cleared",v.cleared);
  io("active",v.active);
  io("cutscene",v.cutscene);
  }

template<class IO> void fields(IO& io, AiHuman& v) {
  io("water_level",v.water_level);
  io("floor_y",v.floor_y);
  io("water_y",v.water_y);
  io("ceil_y",v.ceil_y);
  io("feet_y",v.feet_y);
  io("head_y",v.head_y);
  io("fall_dist_y",v.fall_dist_y);
  io("fall_start_y",v.fall_start_y);
  io("npc",v.npc);
  io("walk_mode",v.walk_mode);
  io("weapon_mode",v.weapon_mode);
  io("wmode_ast",v.wmode_ast);
  io("wmode_select",v.wmode_select);
  io("change_weapon",v.change_weapon);
  io("action_mode",v.action_mode);
  }

template<class IO> void fields(IO& io, AiMove& v) {
  io("vob",v.vob);
  io("owner",v.owner);
  }

template<class IO> void fields(IO& io, VCameraTrajectoryFrame& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("time",v.time);
  io("roll_angle",v.roll_angle);
  io("fov_scale",v.fov_scale);
  io("motion_type",v.motion_type);
  io("motion_type_fov",v.motion_type_fov);
  io("motion_type_roll",v.motion_type_roll);
  io("motion_type_time_scale",v.motion_type_time_scale);
  io("tension",v.tension);
  io("cam_bias",v.cam_bias);
  io("continuity",v.continuity);
  io("time_scale",v.time_scale);
  io("time_fixed",v.time_fixed);
  io("original_pose",v.original_pose);
  }

template<class IO> void fields(IO& io, VCutsceneCamera& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("trajectory_for",v.trajectory_for);
  io("target_trajectory_for",v.target_trajectory_for);
  io("loop_mode",v.loop_mode);
  io("lerp_mode",v.lerp_mode);
  io("ignore_for_vob_rotation",v.ignore_for_vob_rotation);
  io("ignore_for_vob_rotation_target",v.ignore_for_vob_rotation_target);
  io("adapt",v.adapt);
  io("ease_first",v.ease_first);
  io("ease_last",v.ease_last);
  io("total_duration",v.total_duration);
  io("auto_focus_vob",v.auto_focus_vob);
  io("auto_player_movable",v.auto_player_movable);
  io("auto_untrigger_last",v.auto_untrigger_last);
  io("auto_untrigger_last_delay",v.auto_untrigger_last_delay);
  io("position_count",v.position_count);
  io("target_count",v.target_count);
  io("trajectory_frames",v.trajectory_frames);
  io("target_frames",v.target_frames);
  io("paused",v.paused);
  io("started",v.started);
  io("goto_time_mode",v.goto_time_mode);
  io("cs_time",v.cs_time);
  }

template<class IO> void fields(IO& io, LightPreset& v) {
  io("preset",v.preset);
  io("light_type",v.light_type);
  io("range",v.range);
  io("color",v.color);
  io("cone_angle",v.cone_angle);
  io("is_static",v.is_static);
  io("quality",v.quality);
  io("lensflare_fx",v.lensflare_fx);
  io("on",v.on);
  io("range_animation_scale",v.range_animation_scale);
  io("range_animation_fps",v.range_animation_fps);
  io("range_animation_smooth",v.range_animation_smooth);
  io("color_animation_list",v.color_animation_list);
  io("color_animation_fps",v.color_animation_fps);
  io("color_animation_smooth",v.color_animation_smooth);
  io("can_move",v.can_move);
  }

template<class IO> void fields(IO& io, VLight& v) {
  fields(io,static_cast<VirtualObject&>(v));
  fields(io,static_cast<LightPreset&>(v));
  }

template<class IO> void fields(IO& io, VAnimate& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("start_on",v.start_on);
  io("s_is_running",v.s_is_running);
  }

template<class IO> void fields(IO& io, VItem& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("instance",v.instance);
  io("s_amount",v.s_amount);
  io("s_flags",v.s_flags);
  }

template<class IO> void fields(IO& io, VLensFlare& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("fx",v.fx);
  }

template<class IO> void fields(IO& io, VParticleEffectController& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("pfx_name",v.pfx_name);
  io("kill_when_done",v.kill_when_done);
  io("initially_running",v.initially_running);
  }

template<class IO> void fields(IO& io, VMessageFilter& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("target",v.target);
  io("on_trigger",v.on_trigger);
  io("on_untrigger",v.on_untrigger);
  }

template<class IO> void fields(IO& io, VCodeMaster& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("target",v.target);
  io("ordered",v.ordered);
  io("first_false_is_failure",v.first_false_is_failure);
  io("failure_target",v.failure_target);
  io("untriggered_cancels",v.untriggered_cancels);
  io("slaves",v.slaves);
  io("s_num_triggered_slaves",v.s_num_triggered_slaves);
  }

template<class IO> void fields(IO& io, VMoverController& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("target",v.target);
  io("message",v.message);
  io("key",v.key);
  }

template<class IO> void fields(IO& io, VTouchDamage& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("damage",v.damage);
  io("barrier",v.barrier);
  io("blunt",v.blunt);
  io("edge",v.edge);
  io("fire",v.fire);
  io("fly",v.fly);
  io("magic",v.magic);
  io("point",v.point);
  io("fall",v.fall);
  io("repeat_delay_sec",v.repeat_delay_sec);
  io("volume_scale",v.volume_scale);
  io("collision",v.collision);
  }

template<class IO> void fields(IO& io, VEarthquake& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("radius",v.radius);
  io("duration",v.duration);
  io("amplitude",v.amplitude);
  }

template<class IO> void fields(IO& io, VNpc::Talent& v) {
  io("talent",v.talent);
  io("value",v.value);
  io("skill",v.skill);
  }

template<class IO> void fields(IO& io, VNpc::Slot& v) {
  io("used",v.used);
  io("name",v.name);
  io("item",v.item);
  io("in_inventory",v.in_inventory);
  }

template<class IO> void fields(IO& io, VNpc::News& v) {
  io("told",v.told);
  io("spread_time",v.spread_time);
  io("spread_type",v.spread_type);
  io("news_id",v.news_id);
  io("gossip",v.gossip);
  io("guild_victim",v.guild_victim);
  io("witness_name",v.witness_name);
  io("offender_name",v.offender_name);
  io("victim_name",v.victim_name);
  }

template<class IO> void fields(IO& io, VNpc& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("npc_instance",v.npc_instance);
  io("model_scale",v.model_scale);
  io("model_fatness",v.model_fatness);
  io("overlays",v.overlays);
  io("flags",v.flags);
  io("guild",v.guild);
  io("guild_true",v.guild_true);
  io("level",v.level);
  io("xp",v.xp);
  io("xp_next_level",v.xp_next_level);
  io("lp",v.lp);
  io("talents",v.talents);
  io("fight_tactic",v.fight_tactic);
  io("fight_mode",v.fight_mode);
  io("wounded",v.wounded);
  io("mad",v.mad);
  io("mad_time",v.mad_time);
  io("player",v.player);
  io("attributes",v.attributes);
  io("hit_chance",v.hit_chance);
  io("missions",v.missions);
  io("start_ai_state",v.start_ai_state);
  io("aivar",v.aivar);
  io("script_waypoint",v.script_waypoint);
  io("attitude",v.attitude);
  io("attitude_temp",v.attitude_temp);
  io("name_nr",v.name_nr);
  io("move_lock",v.move_lock);
  io("packed",v.packed);
  io("news",v.news);
  io("items",v.items);
  io("slots",v.slots);
  io("current_state_valid",v.current_state_valid);
  io("current_state_name",v.current_state_name);
  io("current_state_index",v.current_state_index);
  io("current_state_is_routine",v.current_state_is_routine);
  io("next_state_valid",v.next_state_valid);
  io("next_state_name",v.next_state_name);
  io("next_state_index",v.next_state_index);
  io("next_state_is_routine",v.next_state_is_routine);
  io("last_ai_state",v.last_ai_state);
  io("has_routine",v.has_routine);
  io("routine_changed",v.routine_changed);
  io("routine_overlay",v.routine_overlay);
  io("routine_overlay_count",v.routine_overlay_count);
  io("walkmode_routine",v.walkmode_routine);
  io("weaponmode_routine",v.weaponmode_routine);
  io("start_new_routine",v.start_new_routine);
  io("ai_state_driven",v.ai_state_driven);
  io("ai_state_pos",v.ai_state_pos);
  io("current_routine",v.current_routine);
  io("respawn",v.respawn);
  io("respawn_time",v.respawn_time);
  io("protection",v.protection);
  io("bs_interruptable_override",v.bs_interruptable_override);
  io("npc_type",v.npc_type);
  io("spell_mana",v.spell_mana);
  io("carry_vob",v.carry_vob);
  io("enemy",v.enemy);
  }

template<class IO> void fields(IO& io, VMovableObject& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("name",v.name);
  io("hp",v.hp);
  io("damage",v.damage);
  io("movable",v.movable);
  io("takable",v.takable);
  io("focus_override",v.focus_override);
  io("material",v.material);
  io("visual_destroyed",v.visual_destroyed);
  io("owner",v.owner);
  io("owner_guild",v.owner_guild);
  io("destroyed",v.destroyed);
  }

template<class IO> void fields(IO& io, VInteractiveObject& v) {
  fields(io,static_cast<VMovableObject&>(v));
  io("state_count",v.state_count);
  io("target",v.target);
  io("item",v.item);
  io("condition_function",v.condition_function);
  io("on_state_change_function",v.on_state_change_function);
  io("rewind",v.rewind);
  }

template<class IO> void fields(IO& io, VFire& v) {
  fields(io,static_cast<VInteractiveObject&>(v));
  io("slot",v.slot);
  io("vob_tree",v.vob_tree);
  }

template<class IO> void fields(IO& io, VContainer& v) {
  fields(io,static_cast<VInteractiveObject&>(v));
  io("locked",v.locked);
  io("key",v.key);
  io("pick_string",v.pick_string);
  io("contents",v.contents);
  io("s_items",v.s_items);
  }

template<class IO> void fields(IO& io, VDoor& v) {
  fields(io,static_cast<VInteractiveObject&>(v));
  io("locked",v.locked);
  io("key",v.key);
  io("pick_string",v.pick_string);
  }

template<class IO> void fields(IO& io, VSound& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("volume",v.volume);
  io("mode",v.mode);
  io("random_delay",v.random_delay);
  io("random_delay_var",v.random_delay_var);
  io("initially_playing",v.initially_playing);
  io("ambient3d",v.ambient3d);
  io("obstruction",v.obstruction);
  io("cone_angle",v.cone_angle);
  io("volume_type",v.volume_type);
  io("radius",v.radius);
  io("sound_name",v.sound_name);
  io("s_is_running",v.s_is_running);
  io("s_is_allowed_to_run",v.s_is_allowed_to_run);
  }

template<class IO> void fields(IO& io, VSoundDaytime& v) {
  fields(io,static_cast<VSound&>(v));
  io("start_time",v.start_time);
  io("end_time",v.end_time);
  io("sound_name2",v.sound_name2);
  }

template<class IO> void fields(IO& io, VTrigger& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("flags",v.flags);
  io("filter_flags",v.filter_flags);
  io("target",v.target);
  io("start_enabled",v.start_enabled);
  io("send_untrigger",v.send_untrigger);
  io("react_to_on_trigger",v.react_to_on_trigger);
  io("react_to_on_touch",v.react_to_on_touch);
  io("react_to_on_damage",v.react_to_on_damage);
  io("respond_to_object",v.respond_to_object);
  io("respond_to_pc",v.respond_to_pc);
  io("respond_to_npc",v.respond_to_npc);
  io("vob_target",v.vob_target);
  io("max_activation_count",v.max_activation_count);
  io("retrigger_delay_sec",v.retrigger_delay_sec);
  io("damage_threshold",v.damage_threshold);
  io("fire_delay_sec",v.fire_delay_sec);
  io("s_next_time_triggerable",v.s_next_time_triggerable);
  io("s_count_can_be_activated",v.s_count_can_be_activated);
  io("s_other_vob",v.s_other_vob);
  io("s_is_enabled",v.s_is_enabled);
  }

template<class IO> void fields(IO& io, VMover& v) {
  fields(io,static_cast<VTrigger&>(v));
  io("behavior",v.behavior);
  io("touch_blocker_damage",v.touch_blocker_damage);
  io("stay_open_time_sec",v.stay_open_time_sec);
  io("locked",v.locked);
  io("auto_link",v.auto_link);
  io("auto_rotate",v.auto_rotate);
  io("speed",v.speed);
  io("lerp_mode",v.lerp_mode);
  io("speed_mode",v.speed_mode);
  io("keyframes",v.keyframes);
  io("sfx_open_start",v.sfx_open_start);
  io("sfx_open_end",v.sfx_open_end);
  io("sfx_transitioning",v.sfx_transitioning);
  io("sfx_close_start",v.sfx_close_start);
  io("sfx_close_end",v.sfx_close_end);
  io("sfx_lock",v.sfx_lock);
  io("sfx_unlock",v.sfx_unlock);
  io("sfx_use_locked",v.sfx_use_locked);
  io("s_act_key_pos_delta",v.s_act_key_pos_delta);
  io("s_act_keyframe_f",v.s_act_keyframe_f);
  io("s_act_keyframe",v.s_act_keyframe);
  io("s_next_keyframe",v.s_next_keyframe);
  io("s_move_speed_unit",v.s_move_speed_unit);
  io("s_advance_dir",v.s_advance_dir);
  io("s_mover_state",v.s_mover_state);
  io("s_trigger_event_count",v.s_trigger_event_count);
  io("s_stay_open_time_dest",v.s_stay_open_time_dest);
  }

template<class IO> void fields(IO& io, VTriggerList::Target& v) {
  io("name",v.name);
  io("delay",v.delay);
  }

template<class IO> void fields(IO& io, VTriggerList& v) {
  fields(io,static_cast<VTrigger&>(v));
  io("mode",v.mode);
  io("targets",v.targets);
  io("s_act_target",v.s_act_target);
  io("s_send_on_trigger",v.s_send_on_trigger);
  }

template<class IO> void fields(IO& io, VTriggerScript& v) {
  fields(io,static_cast<VTrigger&>(v));
  io("function",v.function);
  }

template<class IO> void fields(IO& io, VTriggerChangeLevel& v) {
  fields(io,static_cast<VTrigger&>(v));
  io("level_name",v.level_name);
  io("start_vob",v.start_vob);
  }

template<class IO> void fields(IO& io, VTriggerWorldStart& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("target",v.target);
  io("fire_once",v.fire_once);
  io("s_has_fired",v.s_has_fired);
  }

template<class IO> void fields(IO& io, VTriggerUntouch& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("target",v.target);
  }

template<class IO> void fields(IO& io, VZoneMusic& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("enabled",v.enabled);
  io("priority",v.priority);
  io("ellipsoid",v.ellipsoid);
  io("reverb",v.reverb);
  io("volume",v.volume);
  io("loop",v.loop);
  io("s_local_enabled",v.s_local_enabled);
  io("s_day_entrance_done",v.s_day_entrance_done);
  io("s_night_entrance_done",v.s_night_entrance_done);
  }

template<class IO> void fields(IO& io, VZoneFarPlane& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("vob_far_plane_z",v.vob_far_plane_z);
  io("inner_range_percentage",v.inner_range_percentage);
  }

template<class IO> void fields(IO& io, VZoneFog& v) {
  fields(io,static_cast<VirtualObject&>(v));
  io("range_center",v.range_center);
  io("inner_range_percentage",v.inner_range_percentage);
  io("color",v.color);
  io("fade_out_sky",v.fade_out_sky);
  io("override_color",v.override_color);
  }

template<class IO> void fields(IO& io, ConversationMessageEvent& v) {
  io("type",v.type);
  io("text",v.text);
  io("name",v.name);
  io("high_priority",v.high_priority);
  io("used",v.used);
  io("deleted",v.deleted);
  }

template<class IO> void fields(IO& io, CutsceneAtomicBlock& v) {
  io("message",v.message);
  io("synchronized",v.synchronized);
  }

template<class IO> void fields(IO& io, CutsceneBlock& v) {
  io("name",v.name);
  io("block",v.block);
  }

template<class IO> void fields(IO& io, Cutscene& v) {
  fields(io,static_cast<CutsceneBlock&>(v));
  }

template<class IO> void fields(IO& io, CutsceneProps& v) {
  io("name",v.name);
  io("global",v.global);
  io("loop",v.loop);
  io("has_to_be_triggered",v.has_to_be_triggered);
  io("distance",v.distance);
  io("range",v.range);
  io("locked_block_count",v.locked_block_count);
  io("run_behaviour",v.run_behaviour);
  io("run_behaviour_value",v.run_behaviour_value);
  io("stage_name",v.stage_name);
  io("script_function_on_stop",v.script_function_on_stop);
  }

template<class IO> void fields(IO& io, CutsceneContext& v) {
  fields(io,static_cast<Cutscene&>(v));
  io("props",v.props);
  io("role_count",v.role_count);
  io("role_vob_count",v.role_vob_count);
  io("npc",v.npc);
  io("main_role",v.main_role);
  io("is_cutscene",v.is_cutscene);
  io("reference",v.reference);
  io("actual_block",v.actual_block);
  io("alt_stage",v.alt_stage);
  io("stage",v.stage);
  io("was_triggered",v.was_triggered);
  }

template<class IO> void fields(IO& io, WayPoint& v) {
  io("name",v.name);
  io("water_depth",v.water_depth);
  io("under_water",v.under_water);
  io("position",v.position);
  io("direction",v.direction);
  io("free_point",v.free_point);
  }

template<class IO> void fields(IO& io, WorldChunkSource& v) {
  io("world",v.world);
  io("size",v.size);
  io("crc32",v.crc32);
  }

template<class IO> void fields(IO& io, BspSector& v) {
  io("name",v.name);
  io("node_indices",v.node_indices);
  io("portal_polygon_indices",v.portal_polygon_indices);
  }

template<class IO> void fields(IO& io, WayNet& v) {
  io("points",v.points);
  std::vector<std::array<uint32_t,2>> edges;
  if constexpr(!IO::reading) {
    std::unordered_map<const WayPoint*,uint32_t> indices;
    for(size_t i=0;i<v.points.size();++i) {
      require(v.points[i]!=nullptr,"null waypoint");
      indices.emplace(v.points[i].get(),uint32_t(i));
      }
    for(const auto& e:v.edges) {
      require(indices.count(e.first.get()) && indices.count(e.second.get()),"waypoint edge outside points");
      edges.push_back({indices.at(e.first.get()),indices.at(e.second.get())});
      }
    }
  io("edges",edges);
  if constexpr(IO::reading) {
    for(const auto& e:edges) {
      require(e[0]<v.points.size() && e[1]<v.points.size(),"waypoint edge outside points");
      v.edges.emplace_back(v.points[e[0]],v.points[e[1]]);
      }
    }
  }

// Every VirtualObjectType supported by ZenKit, plus the nested archive objects.
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

template<class IO> void objectFields(IO& io, Object& v) {
  switch(v.get_object_type()) {
#define VISIT(tag,cpp) case ObjectType::tag: { \
    auto* typed=dynamic_cast<cpp*>(&v); \
    require(typed!=nullptr,std::string("class/type mismatch: ")+#tag); \
    fields(io,*typed); return; }
    WORLD_DATA_OBJECTS(VISIT)
#undef VISIT
    default:
      if(typeid(v)==typeid(Visual)) { fields(io,static_cast<Visual&>(v)); return; }
      fail(std::string("unsupported archive class ")+typeid(v).name()+" (type "+
           std::to_string(int(v.get_object_type()))+")");
    }
  }
std::string className(Object& v) {
  if(typeid(v)==typeid(Visual)) return "Visual";
  for(const auto& [value,name]:enumNames<ObjectType>())
    if(value==v.get_object_type()) return name;
  fail(std::string("unknown archive class ")+typeid(v).name());
  }
std::shared_ptr<Object> makeObject(const std::string& name) {
#define MAKE(tag,cpp) if(name==#tag) return std::make_shared<cpp>();
  WORLD_DATA_OBJECTS(MAKE)
#undef MAKE
  if(name=="Visual") return std::make_shared<Visual>();
  fail("unknown archive class "+name);
  }

struct WriteContext { std::unordered_map<const Object*,uint64_t> objects; };
struct ReadContext { std::unordered_map<uint64_t,std::shared_ptr<Object>> objects; };

struct JsonWriter {
  static constexpr bool reading = false;
  WriteContext& context;
  std::string out;

  template<class T> void operator()(const char* name, T& v) {
    if(!out.empty()) out+=",";
    out+=jsonString(name)+":";
    try { out+=value(v); }
    catch(const std::exception& e) { fail(std::string(name)+": "+e.what()); }
    }
  template<class T> std::string record(T& v) {
    JsonWriter writer{context,{}};
    fields(writer,v);
    return "{"+writer.out+"}";
    }
  template<class T> std::string value(T& v) {
    if constexpr(std::is_same_v<T,bool>) return v ? "true" : "false";
    else if constexpr(std::is_integral_v<T>) return std::to_string(v);
    else if constexpr(std::is_enum_v<T>) {
      for(const auto& [e,name]:enumNames<T>()) if(e==v) return jsonString(name);
      return std::to_string(static_cast<std::underlying_type_t<T>>(v));
      }
    else return record(v);
    }
  std::string value(float v) { return floatJson(v); }
  std::string value(std::string& v) { return jsonString(v); }
  template<class T> std::string sequence(T& v) {
    std::string result="[";
    for(auto& x:v) { if(result.size()>1) result+=","; result+=value(x); }
    return result+"]";
    }
  template<class T> std::string value(std::vector<T>& v) { return sequence(v); }
  template<class T, size_t N> std::string value(T (&v)[N]) { return sequence(v); }
  template<class T, size_t N> std::string value(std::array<T,N>& v) { return sequence(v); }
  template<class T> std::string value(std::optional<T>& v) { return v ? value(*v) : "null"; }
  template<class T> std::string value(std::unique_ptr<T>& v) { return v ? value(*v) : "null"; }
  template<class T> std::string value(std::weak_ptr<T>& v) { auto p=v.lock(); return value(p); }
  template<class T> std::string value(std::shared_ptr<T>& v) {
    if(!v) return "null";
    const auto [it,inserted]=context.objects.emplace(v.get(),context.objects.size());
    if(!inserted) return "{\"ref\":"+std::to_string(it->second)+"}";
    JsonWriter writer{context,{}};
    auto id=it->second;
    auto name=className(*v);
    writer("object_id",id);
    writer("class",name);
    objectFields(writer,*v);
    return "{"+writer.out+"}";
    }
  std::string value(Vec2& v) { float a[]={v.x,v.y}; return sequence(a); }
  std::string value(Vec3& v) { float a[]={v.x,v.y,v.z}; return sequence(a); }
  std::string value(Quat& v) { float a[]={v.x,v.y,v.z,v.w}; return sequence(a); }
  std::string value(Color& v) { uint8_t a[]={v.r,v.g,v.b,v.a}; return sequence(a); }
  std::string value(Mat3& v) {
    float rows[3][3];
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) rows[r][c]=v[c][r];
    return sequence(rows);
    }
  std::string value(Mat4& v) {
    float rows[4][4];
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) rows[r][c]=v[c][r];
    return sequence(rows);
    }
  std::string value(std::variant<std::shared_ptr<CutsceneAtomicBlock>,std::shared_ptr<CutsceneBlock>>& v) {
    return std::visit([&](auto& p){ return value(p); },v);
    }
  };

struct JsonReader {
  static constexpr bool reading = true;
  ReadContext& context;
  const Json& in;

  template<class T> void operator()(const char* name, T& v) {
    try { value(member(in,name),v); }
    catch(const std::exception& e) { fail(std::string(name)+": "+e.what()); }
    }
  template<class T> void value(const Json& j, T& v) {
    if constexpr(std::is_same_v<T,bool>) {
      require(j.IsBool(),"expected boolean"); v=j.GetBool();
      }
    else if constexpr(std::is_integral_v<T>) {
      if constexpr(std::is_unsigned_v<T>) {
        require(j.IsUint64() && j.GetUint64()<=std::numeric_limits<T>::max(),"unsigned integer out of range");
        v=static_cast<T>(j.GetUint64());
        } else {
        require(j.IsInt64() && j.GetInt64()>=std::numeric_limits<T>::min() &&
                j.GetInt64()<=std::numeric_limits<T>::max(),"integer out of range");
        v=static_cast<T>(j.GetInt64());
        }
      }
    else if constexpr(std::is_enum_v<T>) {
      if(j.IsString()) {
        for(const auto& [e,name]:enumNames<T>()) if(j==name) { v=e; return; }
        fail("unknown enum name "+std::string(j.GetString()));
        }
      std::underlying_type_t<T> raw;
      value(j,raw);
      v=static_cast<T>(raw);
      }
    else {
      require(j.IsObject(),"expected object");
      JsonReader reader{context,j}; fields(reader,v);
      }
    }
  void value(const Json& j, float& v) {
    require(j.IsNumber(),"expected float");
    v=float(j.GetDouble());
    require(std::isfinite(v),"float out of range");
    }
  void value(const Json& j, std::string& v) {
    require(j.IsString(),"expected string");
    v=gothicglb::jsonToString(j,"string");
    }
  template<class T> void value(const Json& j, std::vector<T>& v) {
    array(j); v.resize(j.Size());
    for(rapidjson::SizeType i=0;i<j.Size();++i) value(j[i],v[i]);
    }
  template<class T, size_t N> void value(const Json& j, T (&v)[N]) {
    array(j,N); for(unsigned i=0;i<N;++i) value(j[i],v[i]);
    }
  template<class T, size_t N> void value(const Json& j, std::array<T,N>& v) {
    array(j,N); for(unsigned i=0;i<N;++i) value(j[i],v[i]);
    }
  template<class T> void value(const Json& j, std::optional<T>& v) {
    if(j.IsNull()) v.reset(); else value(j,v.emplace());
    }
  template<class T> void value(const Json& j, std::unique_ptr<T>& v) {
    if(j.IsNull()) v.reset(); else { v=std::make_unique<T>(); value(j,*v); }
    }
  template<class T> void value(const Json& j, std::weak_ptr<T>& v) {
    std::shared_ptr<T> p; value(j,p); v=p;
    }
  template<class T> void value(const Json& j, std::shared_ptr<T>& v) {
    if(j.IsNull()) { v.reset(); return; }
    require(j.IsObject(),"expected archive object");
    std::shared_ptr<Object> p;
    uint64_t id;
    if(j.HasMember("ref")) {
      value(j["ref"],id);
      require(context.objects.count(id)!=0,"unresolved object reference "+std::to_string(id));
      p=context.objects.at(id);
      } else {
      value(member(j,"object_id"),id);
      std::string name; value(member(j,"class"),name);
      p=makeObject(name);
      require(context.objects.emplace(id,p).second,"duplicate object_id "+std::to_string(id));
      JsonReader reader{context,j}; objectFields(reader,*p);
      }
    v=std::dynamic_pointer_cast<T>(p);
    require(v!=nullptr,"object reference has the wrong class");
    }
  void value(const Json& j, Vec2& v) { array(j,2); value(j[0],v.x); value(j[1],v.y); }
  void value(const Json& j, Vec3& v) { array(j,3); value(j[0],v.x); value(j[1],v.y); value(j[2],v.z); }
  void value(const Json& j, Quat& v) { array(j,4); value(j[0],v.x); value(j[1],v.y); value(j[2],v.z); value(j[3],v.w); }
  void value(const Json& j, Color& v) { array(j,4); value(j[0],v.r); value(j[1],v.g); value(j[2],v.b); value(j[3],v.a); }
  void value(const Json& j, Mat3& v) {
    float rows[3][3]; value(j,rows);
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) v[c][r]=rows[r][c];
    }
  void value(const Json& j, Mat4& v) {
    float rows[4][4]; value(j,rows);
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) v[c][r]=rows[r][c];
    }
  void value(const Json& j, std::variant<std::shared_ptr<CutsceneAtomicBlock>,std::shared_ptr<CutsceneBlock>>& v) {
    std::shared_ptr<Object> p; value(j,p);
    if(auto a=std::dynamic_pointer_cast<CutsceneAtomicBlock>(p)) v=a;
    else {
      auto b=std::dynamic_pointer_cast<CutsceneBlock>(p);
      require(b!=nullptr,"invalid cutscene block"); v=b;
      }
    }
  };

rapidjson::Document document(const std::vector<uint8_t>& bytes, const std::string& what) {
  rapidjson::Document d;
  d.Parse<rapidjson::kParseFullPrecisionFlag|rapidjson::kParseValidateEncodingFlag>(
      reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!d.HasParseError(),what+": JSON error at "+std::to_string(d.GetErrorOffset())+
          ": "+rapidjson::GetParseError_En(d.GetParseError()));
  require(d.IsObject(),what+": expected object");
  const auto& version=member(d,"format");
  require(version.IsInt() && version.GetInt()==1,what+": unsupported format");
  return d;
  }
rapidjson::Document document(const fs::path& path) {
  return document(gothicglb::readFile(path),path.string());
  }
void writeJson(const fs::path& path, const std::string& text) {
  gothicglb::writeFile(path,text.data(),text.size());
  }
JsonWriter header(WriteContext& ctx, WorldChunkSource& source) {
  JsonWriter w{ctx,{}};
  int format=1; w("format",format); w("source",source);
  return w;
  }
void sameSource(const WorldChunkSource& a, const WorldChunkSource& b) {
  require(a.world==b.world && a.size==b.size && a.crc32==b.crc32,"source name, size or CRC32 differs");
  }

double intersect(const Vec3& from,const Vec3& direction,const Triangle& triangle) {
      struct Double3 {
        double x,y,z;
        explicit Double3(const Vec3& v):x(v.x),y(v.y),z(v.z) {}
        Double3(double x,double y,double z):x(x),y(y),z(z) {}
        Double3 operator-(const Double3& b) const { return {x-b.x,y-b.y,z-b.z}; }
        double dot(const Double3& b) const { return x*b.x+y*b.y+z*b.z; }
        Double3 cross(const Double3& b) const { return {y*b.z-z*b.y,z*b.x-x*b.z,x*b.y-y*b.x}; }
        };
      const Double3 start(from),ray(direction),a(triangle.point[0]),b(triangle.point[1]),c(triangle.point[2]);
      const auto edge1=b-a,edge2=c-a,p=ray.cross(edge2);
      const double determinant=edge1.dot(p);
      if(std::abs(determinant)<1.e-9) return -1.;
      const auto offset=start-a;
      const double u=offset.dot(p)/determinant;
      if(u<-.0000001 || u>1.0000001) return -1.;
      const auto q=offset.cross(edge1);
      const double v=ray.dot(q)/determinant;
      if(v<-.0000001 || u+v>1.0000001) return -1.;
      const double distance=edge2.dot(q)/determinant;
      return distance>=0. ? distance : -1.;
      }

} // namespace

bool isIndoor(asset::BspTreeType mode, const std::vector<Room>& rooms, const zenkit::Vec3& point) {
      if(mode==BspTreeType::INDOOR) return true;
      for(const auto& room:rooms) {
        if(point.x<room.min.x || point.x>room.max.x ||
           point.y<room.min.y || point.y>room.max.y ||
           point.z<room.min.z || point.z>room.max.z) continue;
        // Three oblique rays avoid an edge or coincident portal changing the
        // answer. Coincident intersections count once, including back faces.
        const std::array<Vec3,3> directions={Vec3(.371f,.529f,.763f),
                                                   Vec3(-.683f,.413f,.601f),
                                                   Vec3(.227f,.851f,-.473f)};
        unsigned inside=0;
        for(const auto& direction:directions) {
          std::vector<double> intersections;
          for(const auto& triangle:room.triangles) {
            const double distance=intersect(point,direction,triangle);
            if(distance>=0.) intersections.push_back(distance);
            }
          std::sort(intersections.begin(),intersections.end());
          size_t crossings=0;
          double previous=-1.;
          for(const auto distance:intersections) {
            if(previous<0. || distance-previous>.01) ++crossings;
            previous=distance;
            }
          inside+=crossings%2;
          }
        if(inside>=2) return true;
        }
      return false;
  }

namespace {
void writeRooms(const std::vector<Room>& rooms, const fs::path& file) {
  std::vector<uint8_t> bytes{'G','W','R','M'};
  auto u32=[&](uint64_t v) {
    require(v<=UINT32_MAX,"rooms.bin: count exceeds u32");
    for(unsigned i=0;i<4;++i) bytes.push_back(uint8_t(v>>(8*i)));
    };
  auto vec=[&](const Vec3& v) {
    for(unsigned i=0;i<3;++i) u32(std::bit_cast<uint32_t>(v[i]));
    };
  u32(1); u32(rooms.size());
  for(const auto& room:rooms) {
    vec(room.min); vec(room.max); u32(room.triangles.size());
    for(const auto& triangle:room.triangles)
      for(const auto& point:triangle.point) vec(point);
    }
  gothicglb::writeFile(file,bytes.data(),bytes.size());
  }
std::vector<Room> readRooms(const fs::path& file, size_t sectorCount) {
  const auto bytes=gothicglb::readFile(file);
  require(bytes.size()>=12 && std::equal(bytes.begin(),bytes.begin()+4,"GWRM"),"rooms.bin: invalid GWRM header");
  size_t at=4;
  auto u32=[&]() {
    require(at+4<=bytes.size(),"rooms.bin: truncated u32");
    uint32_t v=0;
    for(unsigned i=0;i<4;++i) v|=uint32_t(bytes[at++])<<(8*i);
    return v;
    };
  auto vec=[&](Vec3& v) {
    for(unsigned i=0;i<3;++i) {
      v[i]=std::bit_cast<float>(u32());
      require(std::isfinite(v[i]),"rooms.bin: non-finite coordinate");
      }
    };
  require(u32()==1,"rooms.bin: unsupported version");
  const auto count=u32();
  require(count==sectorCount,"rooms.bin: room/sector count differs");
  require(count<=(bytes.size()-at)/28,"rooms.bin: truncated rooms");
  std::vector<Room> rooms(count);
  for(auto& room:rooms) {
    vec(room.min); vec(room.max);
    const auto triangles=u32();
    require(triangles<=(bytes.size()-at)/36,"rooms.bin: truncated triangles");
    room.triangles.resize(triangles);
    for(auto& triangle:room.triangles)
      for(auto& point:triangle.point) vec(point);
    }
  require(at==bytes.size(),"rooms.bin: trailing bytes");
  return rooms;
  }
void writeBsp(const BspTree& bsp, const fs::path& file) {
  std::vector<uint8_t> bytes{'G','W','B','S'};
  auto u32=[&](uint64_t v) {
    require(v<=UINT32_MAX,"BSP index/count exceeds u32");
    for(unsigned i=0;i<4;++i) bytes.push_back(uint8_t(v>>(8*i)));
    };
  auto f32=[&](float v){ u32(std::bit_cast<uint32_t>(v)); };
  u32(1); u32(bsp.nodes.size());
  for(const auto& n:bsp.nodes) {
    for(unsigned i=0;i<3;++i) f32(n.bbox.min[i]);
    for(unsigned i=0;i<3;++i) f32(n.bbox.max[i]);
    for(unsigned i=0;i<4;++i) f32(n.plane[i]);
    u32(uint32_t(n.front_index)); u32(uint32_t(n.back_index)); u32(uint32_t(n.parent_index));
    bytes.push_back(n.lod);
    }
  u32(bsp.leaf_node_indices.size());
  for(const auto i:bsp.leaf_node_indices) u32(i);
  gothicglb::writeFile(file,bytes.data(),bytes.size());
  }
void readBsp(BspTree& bsp, const fs::path& file) {
  const auto bytes=gothicglb::readFile(file);
  require(bytes.size()>=16 && std::equal(bytes.begin(),bytes.begin()+4,"GWBS"),"bsp.bin: invalid GWBS header");
  size_t at=4;
  auto u32=[&]() {
    require(at+4<=bytes.size(),"bsp.bin: truncated u32");
    uint32_t v=0;
    for(unsigned i=0;i<4;++i) v|=uint32_t(bytes[at++])<<(8*i);
    return v;
    };
  auto f32=[&](){ return std::bit_cast<float>(u32()); };
  require(u32()==1,"bsp.bin: unsupported version");
  const auto count=u32();
  require(count<=(bytes.size()-16)/53,"bsp.bin: truncated nodes");
  bsp.nodes.resize(count);
  for(auto& n:bsp.nodes) {
    for(unsigned i=0;i<3;++i) n.bbox.min[i]=f32();
    for(unsigned i=0;i<3;++i) n.bbox.max[i]=f32();
    for(unsigned i=0;i<4;++i) n.plane[i]=f32();
    n.front_index=std::bit_cast<int32_t>(u32());
    n.back_index=std::bit_cast<int32_t>(u32());
    n.parent_index=std::bit_cast<int32_t>(u32());
    n.lod=bytes.at(at++);
    for(const auto i:{n.front_index,n.back_index,n.parent_index})
      require(i==-1 || (i>=0 && uint32_t(i)<count),"bsp.bin: node reference out of range");
    }
  const auto leaves=u32();
  require(leaves==(bytes.size()-at)/4 && at+uint64_t(leaves)*4==bytes.size(),"bsp.bin: invalid leaf count or trailing bytes");
  for(uint32_t i=0;i<leaves;++i) {
    const auto n=u32();
    require(n<count && bsp.nodes[n].is_leaf(),"bsp.bin: leaf reference is not a leaf node");
    bsp.leaf_node_indices.push_back(n);
    }
  }
} // namespace

void write(const Data& data, const fs::path& dir) {
  fs::create_directories(dir);
  // The field visitor never mutates on write; it shares the explicit schema with the reader.
  auto& d=const_cast<Data&>(data);
  require(d.rooms.size()==d.bsp.sectors.size(),"room/sector count differs");
  {
  WriteContext ctx; auto w=header(ctx,d.source);
  w("vobs",d.vobs);
  writeJson(dir/"vobs.json","{"+w.out+"}\n");
  }
  {
  WriteContext ctx; auto w=header(ctx,d.source);
  w("waynet",d.waynet);
  writeJson(dir/"waynet.json","{"+w.out+"}\n");
  }
  {
  WriteContext ctx; auto w=header(ctx,d.source);
  w("name",d.name); w("bbox",d.bbox); w("mode",d.bsp.mode);
  std::string sectors="[";
  for(size_t i=0;i<d.rooms.size();++i) {
    JsonWriter sector{ctx,{}}; fields(sector,d.bsp.sectors[i]);
    sector("room",i);
    if(i) sectors+=",";
    sectors+="\n{"+sector.out+"}";
    }
  w.out+=",\"sectors\":"+sectors+"]";
  writeJson(dir/"sectors.json","{"+w.out+"}\n");
  }
  writeBsp(d.bsp,dir/"bsp.bin");
  writeRooms(d.rooms,dir/"rooms.bin");
  }

Data read(const fs::path& dir, const fs::path& overrideDir) {
  Data d;
  const auto editable=[&](const char* name) {
    return !overrideDir.empty() && fs::exists(overrideDir/name) ? overrideDir/name : dir/name;
    };
  {
  auto doc=document(editable("vobs.json")); ReadContext ctx; JsonReader r{ctx,doc};
  r("source",d.source); r("vobs",d.vobs);
  }
  {
  auto doc=document(editable("waynet.json")); ReadContext ctx; JsonReader r{ctx,doc};
  WorldChunkSource source; r("source",source); sameSource(d.source,source);
  r("waynet",d.waynet);
  }
  {
  auto doc=document(dir/"sectors.json"); ReadContext ctx; JsonReader r{ctx,doc};
  WorldChunkSource source; r("source",source); sameSource(d.source,source);
  r("name",d.name); r("bbox",d.bbox); r("mode",d.bsp.mode);
  const auto& sectors=member(doc,"sectors"); array(sectors);
  for(const auto& j:sectors.GetArray()) {
    JsonReader sector{ctx,j};
    fields(sector,d.bsp.sectors.emplace_back());
    size_t room; sector("room",room);
    require(room==d.bsp.sectors.size()-1,"sector room index differs from sector order");
    }
  }
  readBsp(d.bsp,dir/"bsp.bin");
  d.rooms=readRooms(dir/"rooms.bin",d.bsp.sectors.size());
  for(const auto& sector:d.bsp.sectors)
    for(const auto leaf:sector.node_indices)
      require(leaf<d.bsp.leaf_node_indices.size(),"sector "+sector.name+": leaf index out of range");
  return d;
  }

namespace {
template<class T> std::string recordDifference(const T& a, const T& b, const std::string& where) {
  WriteContext ac,bc; JsonWriter aw{ac,{}},bw{bc,{}};
  const auto x=aw.value(const_cast<T&>(a)),y=bw.value(const_cast<T&>(b));
  if(x==y) return {};
  size_t at=0;
  while(at<std::min(x.size(),y.size()) && x[at]==y[at]) ++at;
  return where+": JSON byte "+std::to_string(at)+", expected '"+x.substr(at,80)+"', actual '"+y.substr(at,80)+"'";
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
} // namespace

std::string compare(const Data& a, const Data& b) {
  try {
    sameSource(a.source,b.source);
    require(a.name==b.name,"world name differs");
    equalVec(a.bbox.min,b.bbox.min,"world bbox.min"); equalVec(a.bbox.max,b.bbox.max,"world bbox.max");
    const auto vobs=recordDifference(a.vobs,b.vobs,"vobs"); if(!vobs.empty()) return vobs;
    const auto ways=recordDifference(a.waynet,b.waynet,"waynet"); if(!ways.empty()) return ways;
    equalInt(uint32_t(a.bsp.mode),uint32_t(b.bsp.mode),"BSP mode");
    equalInt(a.bsp.nodes.size(),b.bsp.nodes.size(),"BSP node count");
    for(size_t i=0;i<a.bsp.nodes.size();++i) {
      const auto& x=a.bsp.nodes[i]; const auto& y=b.bsp.nodes[i];
      const auto where="BSP node "+std::to_string(i)+" ";
      equalVec(x.bbox.min,y.bbox.min,where+"bbox.min"); equalVec(x.bbox.max,y.bbox.max,where+"bbox.max");
      for(unsigned k=0;k<4;++k) equalFloat(x.plane[k],y.plane[k],where+"plane["+std::to_string(k)+"]");
      equalInt(x.front_index,y.front_index,where+"front_index");
      equalInt(x.back_index,y.back_index,where+"back_index");
      equalInt(x.parent_index,y.parent_index,where+"parent_index"); equalInt(x.lod,y.lod,where+"lod");
      }
    equalInt(a.bsp.leaf_node_indices.size(),b.bsp.leaf_node_indices.size(),"BSP leaf count");
    for(size_t i=0;i<a.bsp.leaf_node_indices.size();++i)
      equalInt(a.bsp.leaf_node_indices[i],b.bsp.leaf_node_indices[i],"BSP leaf_node_indices["+std::to_string(i)+"]");
    equalInt(a.bsp.sectors.size(),b.bsp.sectors.size(),"sector count");
    equalInt(a.rooms.size(),b.rooms.size(),"room count");
    for(size_t i=0;i<a.bsp.sectors.size();++i) {
      const auto& x=a.bsp.sectors[i]; const auto& y=b.bsp.sectors[i];
      const auto where="sector "+std::to_string(i)+" '"+x.name+"' ";
      require(x.name==y.name,where+"name: actual '"+y.name+"'");
      require(x.node_indices==y.node_indices,where+"node_indices differ");
      require(x.portal_polygon_indices==y.portal_polygon_indices,where+"portal_polygon_indices differ");
      const auto& r=a.rooms.at(i); const auto& s=b.rooms.at(i);
      equalVec(r.min,s.min,where+"min"); equalVec(r.max,s.max,where+"max");
      equalInt(r.triangles.size(),s.triangles.size(),where+"triangle count");
      for(size_t t=0;t<r.triangles.size();++t)
        for(unsigned p=0;p<3;++p)
          equalVec(r.triangles[t].point[p],s.triangles[t].point[p],where+"triangle "+std::to_string(t)+" point "+std::to_string(p));
      }
    // Compiled world meshes can carry a zero bbox; the BSP root bounds the world.
    const auto& bounds=a.bsp.nodes.empty() ? a.bbox : a.bsp.nodes.front().bbox;
    const auto nx=uint64_t(std::ceil(double(bounds.max.x-bounds.min.x)/400.));
    const auto nz=uint64_t(std::ceil(double(bounds.max.z-bounds.min.z)/400.));
    for(uint64_t x=0;x<=nx;++x) for(uint64_t z=0;z<=nz;++z) for(unsigned h=0;h<3;++h) {
      const Vec3 p{bounds.min.x+(bounds.max.x-bounds.min.x)*float(double(x)/double(std::max(uint64_t(1),nx))),
                   bounds.min.y+(bounds.max.y-bounds.min.y)*float(h+1)/4.f,
                   bounds.min.z+(bounds.max.z-bounds.min.z)*float(double(z)/double(std::max(uint64_t(1),nz)))};
      const auto expected=isIndoor(a.bsp.mode,a.rooms,p),actual=isIndoor(b.bsp.mode,b.rooms,p);
      require(expected==actual,"isIndoor at "+floatJson(p.x)+","+floatJson(p.y)+","+floatJson(p.z)+
              ": expected "+std::to_string(expected)+", actual "+std::to_string(actual));
      }
    return {};
    }
  catch(const std::exception& e) { return a.source.world+": "+e.what(); }
  }

size_t vobCount(const std::vector<std::shared_ptr<asset::VirtualObject>>& vobs) {
  size_t count=0;
  for(const auto& v:vobs) { require(v!=nullptr,"null vob"); count+=1+vobCount(v->children); }
  return count;
  }
void writeZen(const ZenFile& data, const fs::path& file) {
  auto& d=const_cast<ZenFile&>(data);
  WriteContext ctx; auto w=header(ctx,d.source);
  w("kind",d.kind);
  if(d.kind=="world" || d.kind=="cameras") {
    w("vobs",d.vobs); w("waynet",d.waynet);
    } else if(d.kind=="cutscene_library") {
    w("blocks",d.blocks);
    } else {
    require(d.kind=="light_presets","unknown ZEN kind "+d.kind);
    w("light_presets",d.light_presets);
    }
  fs::create_directories(file.parent_path());
  writeJson(file,"{"+w.out+"}\n");
  }
ZenFile readZen(const fs::path& file) {
  return parseZen(gothicglb::readFile(file),file.string());
  }
ZenFile parseZen(const std::vector<uint8_t>& bytes, const std::string& what) {
  auto doc=document(bytes,what); ReadContext ctx; JsonReader r{ctx,doc};
  ZenFile d; r("source",d.source); r("kind",d.kind);
  if(d.kind=="world" || d.kind=="cameras") {
    r("vobs",d.vobs); r("waynet",d.waynet);
    } else if(d.kind=="cutscene_library") {
    r("blocks",d.blocks);
    for(const auto& block:d.blocks) require(block!=nullptr,"null cutscene library root");
    } else {
    require(d.kind=="light_presets","unknown ZEN kind "+d.kind);
    r("light_presets",d.light_presets);
    }
  return d;
  }
std::string compareZen(const ZenFile& a, const ZenFile& b) {
  try {
    sameSource(a.source,b.source);
    require(a.kind==b.kind,"ZEN kind differs");
    auto diff=recordDifference(a.vobs,b.vobs,"vobs"); if(!diff.empty()) return a.source.world+": "+diff;
    diff=recordDifference(a.waynet,b.waynet,"waynet"); if(!diff.empty()) return a.source.world+": "+diff;
    if(a.kind=="cutscene_library") {
      diff=recordDifference(a.blocks,b.blocks,"blocks"); if(!diff.empty()) return a.source.world+": "+diff;
      }
    return recordDifference(a.light_presets,b.light_presets,"light presets");
    }
  catch(const std::exception& e) { return a.source.world+": "+e.what(); }
  }

} // namespace WorldData
