// Data layouts adapted from ZenKit (Copyright 2021-2024 GothicKit Contributors).
// SPDX-License-Identifier: MIT
#pragma once

#include "material.h"
#include <zenkit/Boxes.hh>
#include <array>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace asset {

// Own world records. Numeric tags and defaults retain the imported base contract;
// these types have no archive loading/saving API or ZenKit object inheritance.

enum class ObjectType {
  zCVob = 0,
  zCVobLevelCompo = 1,
  oCItem = 2,
  oCNpc = 3,
  zCMoverController = 4,
  zCVobScreenFX = 5,
  zCVobStair = 6,
  zCPFXController = 7,
  zCVobAnimate = 8,
  zCVobLensFlare = 9,
  zCVobLight = 10,
  zCVobSpot = 11,
  zCVobStartpoint = 12,
  zCMessageFilter = 13,
  zCCodeMaster = 14,
  zCTriggerWorldStart = 15,
  zCCSCamera = 16,
  zCCamTrj_KeyFrame = 17,
  oCTouchDamage = 18,
  zCTriggerUntouch = 19,
  zCEarthquake = 20,
  oCMOB = 21,
  oCMobInter = 22,
  oCMobBed = 23,
  oCMobFire = 24,
  oCMobLadder = 25,
  oCMobSwitch = 26,
  oCMobWheel = 27,
  oCMobContainer = 28,
  oCMobDoor = 29,
  zCTrigger = 30,
  zCTriggerList = 31,
  oCTriggerScript = 32,
  oCTriggerChangeLevel = 33,
  oCCSTrigger = 34,
  zCMover = 35,
  zCVobSound = 36,
  zCVobSoundDaytime = 37,
  oCZoneMusic = 38,
  oCZoneMusicDefault = 39,
  zCZoneZFog = 40,
  zCZoneZFogDefault = 41,
  zCZoneVobFarPlane = 42,
  zCZoneVobFarPlaneDefault = 43,
  ignored = 44,
  unknown = 45,
  oCNpcTalent,
  zCEventManager,
  zCDecal,
  zCMesh,
  zCProgMeshProto,
  zCParticleFX,
  zCAICamera,
  zCModel,
  zCMorphMesh,
  oCAIHuman,
  oCAIVobMove,
  oCCSPlayer,
  zCSkyControler_Outdoor,
  oCWorld,
  zCWayNet,
  zCWaypoint,
  zCMaterial,
  oCSavegameInfo,
  oCCSManager,
  zCCSPoolItem,
  zCCSBlock,
  zCCutscene,
  zCCSCutsceneContext,
  oCMsgConversation,
  zCCSAtomicBlock,
  zCCSLib,
  zCCSProps,
};

struct Object {
  virtual ~Object() = default;
  virtual ObjectType get_object_type() const { return ObjectType::unknown; }
};

struct CutsceneContext;
struct VNpc;

enum class ShadowType : std::uint8_t {
  NONE = 0,
  BLOB = 1,
};

enum class VisualType : std::uint8_t {
  DECAL = 0,
  MESH = 1,
  MULTI_RESOLUTION_MESH = 2,
  PARTICLE_EFFECT = 3,
  AI_CAMERA = 4,
  MODEL = 5,
  MORPH_MESH = 6,
  UNKNOWN = 7,
};

enum class SpriteAlignment : std::uint8_t {
  NONE = 0,
  YAW = 1,
  FULL = 2,
};

enum class AnimationType : std::uint8_t {
  NONE = 0,
  WIND = 1,
  WIND_ALT = 2,
};

struct Visual : Object {
  VisualType type = VisualType::UNKNOWN;
  std::string name;
};

struct VisualMesh final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCMesh; }
};

struct VisualMultiResolutionMesh final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCProgMeshProto; }
};

struct VisualParticleEffect final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCParticleFX; }
};

struct VisualCamera final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCAICamera; }
};

struct VisualModel final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCModel; }
};

struct VisualMorphMesh final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCMorphMesh; }
};

struct VisualDecal final : Visual {
  ObjectType get_object_type() const override { return ObjectType::zCDecal; }
  zenkit::Vec2 dimension {};
  zenkit::Vec2 offset {};
  bool two_sided {};
  AlphaFunction alpha_func {};
  float texture_anim_fps {};
  std::uint8_t alpha_weight {};
  bool ignore_daylight {};
};

struct RigidBody {
  zenkit::Vec3 vel;
  std::uint8_t mode;
  bool gravity_enabled;
  float gravity_scale;
  zenkit::Vec3 slide_direction;
};

struct EventManager final : Object {
  ObjectType get_object_type() const override { return ObjectType::zCEventManager; }
  bool cleared = false;
  bool active = false;
  std::weak_ptr<CutsceneContext> cutscene;
};

struct MoverKeyframe { zenkit::Vec3 position; zenkit::Quat rotation; };

struct VirtualObject;

struct Ai : Object {};

struct AiHuman final : Ai {
  ObjectType get_object_type() const override { return ObjectType::oCAIHuman; }
  int water_level;
  float floor_y;
  float water_y;
  float ceil_y;
  float feet_y;
  float head_y;
  float fall_dist_y;
  float fall_start_y;
  std::weak_ptr<VNpc> npc;
  int walk_mode;
  int weapon_mode;
  int wmode_ast;
  int wmode_select;
  bool change_weapon;
  int action_mode;
};

struct AiMove final : Ai {
  ObjectType get_object_type() const override { return ObjectType::oCAIVobMove; }
  std::weak_ptr<VirtualObject> vob;
  std::weak_ptr<VNpc> owner;
};

enum class VirtualObjectType {
  UNKNOWN = -1,
  zCVob = 0,
  zCVobLevelCompo = 1,
  oCItem = 2,
  oCNpc = 3,
  zCMoverController = 4,
  zCVobScreenFX = 5,
  zCVobStair = 6,
  zCPFXController = 7,
  zCVobAnimate = 8,
  zCVobLensFlare = 9,
  zCVobLight = 10,
  zCVobSpot = 11,
  zCVobStartpoint = 12,
  zCMessageFilter = 13,
  zCCodeMaster = 14,
  zCTriggerWorldStart = 15,
  zCCSCamera = 16,
  zCCamTrj_KeyFrame = 17,
  oCTouchDamage = 18,
  zCTriggerUntouch = 19,
  zCEarthquake = 20,
  oCMOB = 21,
  oCMobInter = 22,
  oCMobBed = 23,
  oCMobFire = 24,
  oCMobLadder = 25,
  oCMobSwitch = 26,
  oCMobWheel = 27,
  oCMobContainer = 28,
  oCMobDoor = 29,
  zCTrigger = 30,
  zCTriggerList = 31,
  oCTriggerScript = 32,
  oCTriggerChangeLevel = 33,
  oCCSTrigger = 34,
  zCMover = 35,
  zCVobSound = 36,
  zCVobSoundDaytime = 37,
  oCZoneMusic = 38,
  oCZoneMusicDefault = 39,
  zCZoneZFog = 40,
  zCZoneZFogDefault = 41,
  zCZoneVobFarPlane = 42,
  zCZoneVobFarPlaneDefault = 43,
};

struct VirtualObject : Object {
  ObjectType get_object_type() const override { return ObjectType::zCVob; }
  VirtualObject() = default;
  VirtualObject(VirtualObject const&) = default;
  VirtualObject(VirtualObject&&) = default;
  ~VirtualObject() override = default;
  VirtualObjectType type = VirtualObjectType::UNKNOWN;
  uint32_t id = 0;
  zenkit::AxisAlignedBoundingBox bbox = zenkit::AxisAlignedBoundingBox::zero();
  zenkit::Vec3 position = zenkit::Vec3 {0};
  zenkit::Mat3 rotation = zenkit::Mat3::identity();
  bool show_visual = false;
  SpriteAlignment sprite_camera_facing_mode = SpriteAlignment::NONE;
  bool cd_static = true;
  bool cd_dynamic = true;
  bool vob_static = false;
  ShadowType dynamic_shadows = ShadowType::NONE;
  bool physics_enabled = true;
  AnimationType anim_mode = AnimationType::NONE;
  std::int32_t bias = 0;
  bool ambient = false;
  float anim_strength = 0;
  float far_clip_scale = 2.0;
  std::string preset_name;
  std::string vob_name;
  std::string visual_name {};
  VisualType associated_visual_type {};
  std::optional<VisualDecal> visual_decal {};
  std::shared_ptr<Visual> visual = nullptr;
  std::shared_ptr<Ai> ai = nullptr;
  std::shared_ptr<EventManager> event_manager = nullptr;
  std::uint8_t sleep_mode = 0;
  float next_on_timer = 0;
  std::optional<RigidBody> rigid_body {};
  std::vector<std::shared_ptr<VirtualObject>> children {};
};

struct VSpot final : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobSpot; }
};

struct VStair final : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobStair; }
};

struct VStartPoint final : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobStartpoint; }
};

struct VLevel final : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobLevelCompo; }
};

enum class CameraMotion : std::uint32_t {
  UNDEFINED = 0,
  SMOOTH = 1,
  LINEAR = 2,
  STEP = 3,
  SLOW = 4,
  FAST = 5,
  CUSTOM = 6,
};

enum class CameraCoordinateReference : std::uint32_t {
  WORLD = 0,
  OBJECT = 1,
};

enum class CameraLerpType : std::uint32_t {
  UNDEFINED = 0,
  PATH = 1,
  PATH_IGNORE_ROLL = 2,
  PATH_ROTATION_SAMPLES = 3,
};

enum class CameraLoop : std::uint32_t {
  NONE = 0,
  RESTART = 1,
  PINGPONG = 2,
};

struct VCameraTrajectoryFrame : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCCamTrj_KeyFrame; }
  float time;
  float roll_angle;
  float fov_scale;
  CameraMotion motion_type;
  CameraMotion motion_type_fov;
  CameraMotion motion_type_roll;
  CameraMotion motion_type_time_scale;
  float tension;
  float cam_bias;
  float continuity;
  float time_scale;
  bool time_fixed;
  zenkit::Mat4 original_pose;
  };

struct VCutsceneCamera : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCCSCamera; }
  CameraCoordinateReference trajectory_for;
  CameraCoordinateReference target_trajectory_for;
  CameraLoop loop_mode;
  CameraLerpType lerp_mode;
  bool ignore_for_vob_rotation;
  bool ignore_for_vob_rotation_target;
  bool adapt;
  bool ease_first;
  bool ease_last;
  float total_duration;
  std::string auto_focus_vob;
  bool auto_player_movable;
  bool auto_untrigger_last;
  float auto_untrigger_last_delay;
  std::int32_t position_count;
  std::int32_t target_count;
  std::vector<std::shared_ptr<VCameraTrajectoryFrame>> trajectory_frames;
  std::vector<std::shared_ptr<VCameraTrajectoryFrame>> target_frames;
  bool paused {false};
  bool started {false};
  bool goto_time_mode {false};
  float cs_time {0};
};

enum class LightType : std::uint32_t {
  POINT = 0,
  SPOT = 1,
  RESERVED0 = 2,
  RESERVED1 = 3,
};

enum class LightQuality : std::uint32_t {
  HIGH = 0,
  MEDIUM = 1,
  LOW = 2,
};

struct LightPreset {
  std::string preset {};
  LightType light_type {LightType::SPOT};
  float range {};
  zenkit::Color color {};
  float cone_angle {0};
  bool is_static {false};
  LightQuality quality {LightQuality::MEDIUM};
  std::string lensflare_fx {};
  bool on {false};
  std::vector<float> range_animation_scale {};
  float range_animation_fps {0};
  bool range_animation_smooth {true};
  std::vector<zenkit::Color> color_animation_list {};
  float color_animation_fps {0};
  bool color_animation_smooth {true};
  bool can_move {true};
};

struct VLight : VirtualObject, LightPreset {
  ObjectType get_object_type() const override { return ObjectType::zCVobLight; }
};

enum class MessageFilterAction : uint32_t {
  NONE = 0,
  TRIGGER = 1,
  UNTRIGGER = 2,
  ENABLE = 3,
  DISABLE = 4,
  TOGGLE = 5,
};

enum class MoverMessageType : uint32_t {
  FIXED_DIRECT = 0,
  FIXED_ORDER = 1,
  NEXT = 2,
  PREVIOUS = 3,
};

enum class TouchCollisionType : std::uint32_t {
  NONE = 0,
  BOX = 1,
  POINT = 2,
};

struct VAnimate : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobAnimate; }
  bool start_on {false};
  bool s_is_running {false};
};

struct VItem : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::oCItem; }
  std::string instance;
  int s_amount;
  int s_flags;
};

struct VLensFlare : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobLensFlare; }
  std::string fx;
};

struct VParticleEffectController : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCPFXController; }
  std::string pfx_name;
  bool kill_when_done;
  bool initially_running;
  };

struct VMessageFilter : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCMessageFilter; }
  std::string target;
  MessageFilterAction on_trigger;
  MessageFilterAction on_untrigger;
};

struct VCodeMaster : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCCodeMaster; }
  std::string target;
  bool ordered;
  bool first_false_is_failure;
  std::string failure_target;
  bool untriggered_cancels;
  std::vector<std::string> slaves;
  uint8_t s_num_triggered_slaves;
};

struct VMoverController : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCMoverController; }
  std::string target;
  MoverMessageType message;
  std::int32_t key;
};

struct VTouchDamage : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::oCTouchDamage; }
  float damage;
  bool barrier;
  bool blunt;
  bool edge;
  bool fire;
  bool fly;
  bool magic;
  bool point;
  bool fall;
  float repeat_delay_sec;
  float volume_scale;
  TouchCollisionType collision;
};

struct VEarthquake : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCEarthquake; }
  float radius;
  float duration;
  zenkit::Vec3 amplitude;
};

struct VNpc : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::oCNpc; }
  static constexpr std::uint32_t attribute_count = 8;
  static constexpr std::uint32_t hcs_count = 4;
  static constexpr std::uint32_t missions_count = 5;
  static constexpr std::uint32_t aivar_count = 100;
  static constexpr std::uint32_t packed_count = 9;
  static constexpr std::uint32_t protection_count = 8;
  struct Talent : Object {
    static constexpr ObjectType TYPE = ObjectType::oCNpcTalent;
    int talent;
    int value;
    int skill;
    [[nodiscard]] ObjectType get_object_type() const override {
      return TYPE;
    }
  };
  struct Slot {
    bool used;
    std::string name;
    std::shared_ptr<VItem> item {};
    bool in_inventory;
  };
  enum class NewsId {
    MURDER = 200,
    ATTACK = 195,
    THEFT = 190,
    DEFEAT = 185,
    NERVE = 180,
    INTERFERE = 175,
    HAS_DEFEATED = 170,
  };
  enum class NewsSpread {
    DONT_SPREAD = 0,
    FRIENDLY_TOWARDS_VICTIM = 1,
    FRIENDLY_TOWARDS_WITNESS = 2,
    FRIENDLY_TOWARDS_OFFENDER = 3,
    SAME_GUILD_VICTIM = 4,
  };
  struct News {
    bool told;
    float spread_time;
    NewsSpread spread_type;
    NewsId news_id;
    bool gossip;
    bool guild_victim;
    std::string witness_name;
    std::string offender_name;
    std::string victim_name;
  };
  std::string npc_instance;
  zenkit::Vec3 model_scale;
  float model_fatness;
  std::vector<std::string> overlays;
  int flags;
  int guild;
  int guild_true;
  int level;
  int xp;
  int xp_next_level;
  int lp;
  std::vector<std::shared_ptr<Talent>> talents;
  int fight_tactic;
  int fight_mode;
  bool wounded;
  bool mad;
  int mad_time;
  bool player;
  int attributes[attribute_count];
  int hit_chance[hcs_count];
  int missions[missions_count];
  std::string start_ai_state;
  int aivar[aivar_count];
  std::string script_waypoint;
  int attitude;
  int attitude_temp;
  int name_nr;
  bool move_lock;
  std::string packed[packed_count];
  std::vector<std::unique_ptr<News>> news;
  std::vector<std::shared_ptr<VItem>> items;
  std::vector<std::unique_ptr<Slot>> slots;
  bool current_state_valid;
  std::string current_state_name;
  int current_state_index;
  bool current_state_is_routine;
  bool next_state_valid;
  std::string next_state_name;
  int next_state_index;
  bool next_state_is_routine;
  int last_ai_state;
  bool has_routine;
  bool routine_changed;
  bool routine_overlay;
  int routine_overlay_count;
  int walkmode_routine;
  bool weaponmode_routine;
  bool start_new_routine;
  int ai_state_driven;
  zenkit::Vec3 ai_state_pos;
  std::string current_routine;
  bool respawn;
  int respawn_time;
  int protection[protection_count];
  int bs_interruptable_override {0};
  int npc_type {0};
  int spell_mana {0};
  std::shared_ptr<VirtualObject> carry_vob;
  std::shared_ptr<VirtualObject> enemy;
};

struct VScreenEffect : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobScreenFX; }
};

enum class SoundMaterialType : std::uint32_t {
  WOOD = 0,
  STONE = 1,
  METAL = 2,
  LEATHER = 3,
  CLAY = 4,
  GLASS = 5,
};

struct VMovableObject : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::oCMOB; }
  std::string name;
  std::int32_t hp;
  std::int32_t damage;
  bool movable;
  bool takable;
  bool focus_override;
  SoundMaterialType material;
  std::string visual_destroyed;
  std::string owner;
  std::string owner_guild;
  bool destroyed;
};

struct VInteractiveObject : VMovableObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobInter; }
  std::int32_t state_count;
  std::string target;
  std::string item;
  std::string condition_function;
  std::string on_state_change_function;
  bool rewind;
};

struct VFire : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobFire; }
  std::string slot;
  std::string vob_tree;
};

struct VContainer : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobContainer; }
  bool locked;
  std::string key;
  std::string pick_string;
  std::string contents;
  std::vector<std::shared_ptr<VItem>> s_items;
};

struct VDoor : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobDoor; }
  bool locked;
  std::string key;
  std::string pick_string;
};

struct VLadder : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobLadder; }
};

struct VSwitch : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobSwitch; }
};

struct VWheel : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobWheel; }
};

struct VBed : VInteractiveObject {
  ObjectType get_object_type() const override { return ObjectType::oCMobBed; }
};

enum class SoundMode : uint32_t {
  LOOP = 0,
  ONCE = 1,
  RANDOM = 2,
};

enum class SoundTriggerVolumeType : uint32_t {
  SPHERICAL = 0,
  ELLIPSOIDAL = 1,
};

struct VSound : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCVobSound; }
  float volume {0};
  SoundMode mode {SoundMode::ONCE};
  float random_delay {0};
  float random_delay_var {0};
  bool initially_playing {false};
  bool ambient3d {false};
  bool obstruction {true};
  float cone_angle {0};
  SoundTriggerVolumeType volume_type {SoundTriggerVolumeType::SPHERICAL};
  float radius {0};
  std::string sound_name {};
  bool s_is_running;
  bool s_is_allowed_to_run;
};

struct VSoundDaytime : VSound {
  ObjectType get_object_type() const override { return ObjectType::zCVobSoundDaytime; }
  float start_time {0};
  float end_time {0};
  std::string sound_name2 {};
};

enum class MoverBehavior : uint32_t {
  TOGGLE = 0,
  TRIGGER_CONTROL = 1,
  OPEN_TIME = 2,
  LOOP = 3,
  SINGLE_KEYS = 4,
};

enum class MoverLerpType : uint32_t {
  LINEAR = 0,
  CURVE = 1,
};

enum class MoverSpeedType : uint32_t {
  CONSTANT = 0,
  SLOW_START_END = 1,
  SLOW_START = 2,
  SLOW_END = 3,
  SEGMENT_SLOW_START_END = 4,
  SEGMENT_SLOW_START = 5,
  SEGMENT_SLOW_END = 6,
};

enum class TriggerBatchMode {
  ALL = 0,
  NEXT = 1,
  RANDOM = 2,
};

struct VTrigger : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCTrigger; }
  std::string target;
  std::uint8_t flags;
  bool start_enabled;
  bool send_untrigger;
  std::uint8_t filter_flags;
  bool react_to_on_trigger;
  bool react_to_on_touch;
  bool react_to_on_damage;
  bool respond_to_object;
  bool respond_to_pc;
  bool respond_to_npc;
  std::string vob_target;
  std::int32_t max_activation_count;
  float retrigger_delay_sec;
  float damage_threshold;
  float fire_delay_sec;
  float s_next_time_triggerable {0};
  int s_count_can_be_activated {0};
  std::shared_ptr<VirtualObject> s_other_vob {nullptr};
  bool s_is_enabled {true};
};

struct VCutsceneTrigger : VTrigger {
  ObjectType get_object_type() const override { return ObjectType::oCCSTrigger; }
};

struct VMover : VTrigger {
  ObjectType get_object_type() const override { return ObjectType::zCMover; }
  MoverBehavior behavior {MoverBehavior::TOGGLE};
  float touch_blocker_damage {0};
  float stay_open_time_sec {0};
  bool locked {true};
  bool auto_link {false};
  bool auto_rotate {false};
  float speed {0};
  MoverLerpType lerp_mode {MoverLerpType::CURVE};
  MoverSpeedType speed_mode {MoverSpeedType::CONSTANT};
  std::vector<MoverKeyframe> keyframes {};
  std::string sfx_open_start {};
  std::string sfx_open_end {};
  std::string sfx_transitioning {};
  std::string sfx_close_start {};
  std::string sfx_close_end {};
  std::string sfx_lock {};
  std::string sfx_unlock {};
  std::string sfx_use_locked {};
  zenkit::Vec3 s_act_key_pos_delta;
  float s_act_keyframe_f;
  int s_act_keyframe;
  int s_next_keyframe;
  float s_move_speed_unit;
  float s_advance_dir;
  uint32_t s_mover_state;
  int s_trigger_event_count;
  float s_stay_open_time_dest;
};

struct VTriggerList : VTrigger {
  ObjectType get_object_type() const override { return ObjectType::zCTriggerList; }
  struct Target {
    std::string name {};
    float delay {};
    [[nodiscard]] bool operator==(Target const& tgt) const noexcept {
      return this->name == tgt.name && this->delay == tgt.delay;
    }
  };
  TriggerBatchMode mode {};
  std::vector<Target> targets {};
  uint8_t s_act_target {0};
  bool s_send_on_trigger {false};
};

struct VTriggerScript : VTrigger {
  ObjectType get_object_type() const override { return ObjectType::oCTriggerScript; }
  std::string function {};
};

struct VTriggerChangeLevel : VTrigger {
  ObjectType get_object_type() const override { return ObjectType::oCTriggerChangeLevel; }
  std::string level_name {};
  std::string start_vob {};
  };

struct VTriggerWorldStart : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCTriggerWorldStart; }
  std::string target;
  bool fire_once;
  bool s_has_fired {false};
};

struct VTriggerUntouch : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCTriggerUntouch; }
  std::string target;
};

struct VZoneMusic : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::oCZoneMusic; }
  bool enabled {false};
  std::int32_t priority {0};
  bool ellipsoid {false};
  float reverb {0};
  float volume {0};
  bool loop {false};
  bool s_local_enabled {true};
  bool s_day_entrance_done {false};
  bool s_night_entrance_done {false};
};

struct VZoneMusicDefault : VZoneMusic {
  ObjectType get_object_type() const override { return ObjectType::oCZoneMusicDefault; }
};

struct VZoneFarPlane : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCZoneVobFarPlane; }
  float vob_far_plane_z;
  float inner_range_percentage;
};

struct VZoneFarPlaneDefault : VZoneFarPlane {
  ObjectType get_object_type() const override { return ObjectType::zCZoneVobFarPlaneDefault; }
};

struct VZoneFog : VirtualObject {
  ObjectType get_object_type() const override { return ObjectType::zCZoneZFog; }
  float range_center {0};
  float inner_range_percentage {0};
  zenkit::Color color {};
  bool fade_out_sky {false};
  bool override_color {false};
};

struct VZoneFogDefault : VZoneFog {
  ObjectType get_object_type() const override { return ObjectType::zCZoneZFogDefault; }
};

struct WayPoint : Object {
  ObjectType get_object_type() const override { return ObjectType::zCWaypoint; }
  std::string name;
  std::int32_t water_depth;
  bool under_water;
  zenkit::Vec3 position;
  zenkit::Vec3 direction;
  bool free_point {false};
};

struct WayNet : Object {
  ObjectType get_object_type() const override { return ObjectType::zCWayNet; }
  std::vector<std::shared_ptr<WayPoint>> points;
  std::vector<std::pair<std::shared_ptr<WayPoint>, std::shared_ptr<WayPoint>>> edges;
};

struct VNpc;

struct ConversationMessageEvent final : Object {
  ObjectType get_object_type() const override { return ObjectType::oCMsgConversation; }
  uint32_t type = 0;
  std::string text;
  std::string name;
  bool high_priority = false;
  bool used = false;
  bool deleted = false;
};

struct CutsceneAtomicBlock final : Object {
  ObjectType get_object_type() const override { return ObjectType::zCCSAtomicBlock; }
  std::shared_ptr<ConversationMessageEvent> message = std::make_shared<ConversationMessageEvent>();
  bool synchronized = false;
};

struct CutsceneBlock : Object {
  ObjectType get_object_type() const override { return ObjectType::zCCSBlock; }
  std::string name;
  std::variant<std::shared_ptr<CutsceneAtomicBlock>, std::shared_ptr<CutsceneBlock>> block =
      std::make_shared<CutsceneAtomicBlock>();
};

struct CutsceneProps final : Object {
  ObjectType get_object_type() const override { return ObjectType::zCCSProps; }
  std::string name;
  bool global;
  bool loop;
  bool has_to_be_triggered;
  float distance;
  float range;
  int locked_block_count;
  uint32_t run_behaviour;
  int run_behaviour_value;
  std::string stage_name;
  std::string script_function_on_stop;
};

struct Cutscene : CutsceneBlock {
  ObjectType get_object_type() const override { return ObjectType::zCCutscene; }
  Cutscene() { block = std::make_shared<CutsceneBlock>(); }
};

struct CutsceneContext final : Cutscene {
  ObjectType get_object_type() const override { return ObjectType::zCCSCutsceneContext; }
  std::shared_ptr<CutsceneProps> props;
  int role_count;
  int role_vob_count;
  std::weak_ptr<VNpc> npc;
  std::weak_ptr<VNpc> main_role;
  bool is_cutscene;
  int reference;
  int actual_block;
  std::shared_ptr<Object> alt_stage;
  std::shared_ptr<Object> stage;
  bool was_triggered;
};

enum class BspTreeType : uint32_t { INDOOR=0, OUTDOOR=1 };

struct BspNode {
zenkit::Vec4 plane;
zenkit::AxisAlignedBoundingBox bbox;
int32_t front_index = -1;
int32_t back_index = -1;
int32_t parent_index = -1;
uint8_t lod = 0;
bool is_leaf() const { return front_index == -1 && back_index == -1; }
};

struct BspSector {
std::string name;
std::vector<uint32_t> node_indices;
std::vector<uint32_t> portal_polygon_indices;
};

// The base retains node topology and room sectors, not archive mesh polygons.

struct BspTree {
BspTreeType mode {};
std::vector<BspNode> nodes;
std::vector<BspSector> sectors;
std::vector<uint64_t> leaf_node_indices;
};
}
