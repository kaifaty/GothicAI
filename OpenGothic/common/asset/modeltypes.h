#pragma once

#include "staticmesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace asset {

struct ModelDate {
  uint32_t year;
  uint16_t month;
  uint16_t day;
  uint16_t hour;
  uint16_t minute;
  uint16_t second;
  };

struct ModelHierarchyNode {
  int16_t parent_index;
  std::string name;
  zenkit::Mat4 transform;
  };

struct ModelHierarchy {
  std::vector<ModelHierarchyNode> nodes {};
  zenkit::AxisAlignedBoundingBox bbox {};
  zenkit::AxisAlignedBoundingBox collision_bbox {};
  zenkit::Vec3 root_translation {};
  uint32_t checksum;
  ModelDate source_date;
  std::string source_path;
  };

struct AnimationSample {
  zenkit::Vec3 position;
  zenkit::Quat rotation;

  bool operator==(const AnimationSample&) const noexcept = default;
  };

struct QuantizedAnimationSample {
  uint16_t position[3];
  uint16_t rotation[3];
  };

enum class AnimationEventType : uint32_t {
  TAG = 0,
  SOUND_EFFECT = 1,
  SOUND_EFFECT_GROUND = 2,
  BATCH = 3,
  SWAP_MESH = 4,
  HEADING = 5,
  PARTICLE_EFFECT = 6,
  PARTICLE_EFFECT_GROUND = 7,
  PARTICLE_EFFECT_STOP = 8,
  SET_MESH = 9,
  MORPH_MESH_ANIMATION = 10,
  CAMERA_TREMOR = 11
  };

struct AnimationEvent {
  static constexpr auto VMAX = 4;
  AnimationEventType type;
  uint32_t frame;
  std::string tag;
  std::string content[VMAX];
  float values[VMAX];
  float probability;
  };

struct ModelAnimation {
  std::string name {};
  std::string next {};
  uint32_t layer {};
  uint32_t frame_count {};
  uint32_t node_count {};
  float fps {};
  float fps_source {};
  float sample_position_min {};
  float sample_position_scale {};
  zenkit::AxisAlignedBoundingBox bbox {};
  uint32_t checksum {};
  std::string source_path {};
  ModelDate source_date {};
  std::string source_script {};
  std::vector<AnimationSample> samples {};
  std::vector<AnimationEvent> events {};
  std::vector<uint32_t> node_indices;
  };

enum class MdsEventType : uint8_t {
  UNKNOWN = 0,
  ITEM_CREATE = 1,
  ITEM_INSERT = 2,
  ITEM_REMOVE = 3,
  ITEM_DESTROY = 4,
  ITEM_PLACE = 5,
  ITEM_EXCHANGE = 6,
  SET_FIGHT_MODE = 7,
  MUNITION_PLACE = 8,
  MUNITION_REMOVE = 9,
  SOUND_DRAW = 10,
  SOUND_UNDRAW = 11,
  MESH_SWAP = 12,
  TORCH_DRAW = 13,
  TORCH_INVENTORY = 14,
  TORCH_DROP = 15,
  HIT_LIMB = 16,
  HIT_DIRECTION = 17,
  DAMAGE_MULTIPLIER = 18,
  PARRY_FRAME = 19,
  OPTIMAL_FRAME = 20,
  HIT_END = 21,
  COMBO_WINDOW = 22
  };

enum class MdsFightMode : uint8_t {
  FIST = 0,
  SINGLE_HANDED = 1,
  DUAL_HANDED = 2,
  BOW = 3,
  CROSSBOW = 4,
  MAGIC = 5,
  NONE = 6,
  INVALID = 0xFF
  };

enum class AnimationFlags : uint8_t {
  NONE = 0,
  MOVE = 1,
  ROTATE = 2,
  QUEUE = 4,
  FLY = 8,
  IDLE = 16,
  INPLACE = 32
  };

constexpr bool operator&(AnimationFlags a, AnimationFlags b) {
  return (uint8_t(a)&uint8_t(b))!=0;
  }

constexpr AnimationFlags operator|(AnimationFlags a, AnimationFlags b) {
  return AnimationFlags(uint8_t(a)|uint8_t(b));
  }

constexpr AnimationFlags& operator|=(AnimationFlags& a, AnimationFlags b) {
  return a = a|b;
  }

enum class AnimationDirection : uint8_t {
  FORWARD = 0,
  BACKWARD = 1
  };

struct MdsSkeleton {
  std::string name;
  bool disable_mesh {false};
  };

struct MdsModelTag {
  std::string bone;
  };

struct MdsEventTag {
  int32_t frame;
  MdsEventType type;
  std::string slot {};
  std::string slot2 {};
  std::string item {};
  std::vector<int32_t> frames {};
  MdsFightMode fight_mode {MdsFightMode::NONE};
  bool attached {false};
  };

struct MdsParticleEffect {
  int32_t frame;
  int32_t index {0};
  std::string name;
  std::string position;
  bool attached {false};
  };

struct MdsCameraTremor {
  int32_t frame {0};
  int32_t field1 {0};
  int32_t field2 {0};
  int32_t field3 {0};
  int32_t field4 {0};
  };

struct MdsParticleEffectStop {
  int32_t frame;
  int32_t index;
  };

struct MdsSoundEffect {
  int32_t frame;
  std::string name;
  float range {1000.0f};
  bool empty_slot {false};
  };

struct MdsSoundEffectGround {
  int32_t frame;
  std::string name;
  float range {1000.0f};
  bool empty_slot {false};
  };

struct MdsMorphAnimation {
  int32_t frame;
  std::string animation;
  std::string node {};
  };

struct MdsAnimation {
  std::string name;
  uint32_t layer;
  std::string next;
  float blend_in;
  float blend_out;
  AnimationFlags flags {AnimationFlags::NONE};
  std::string model;
  AnimationDirection direction;
  int32_t first_frame;
  int32_t last_frame;
  float fps;
  float speed;
  float collision_volume_scale;
  std::vector<MdsEventTag> events {};
  std::vector<MdsParticleEffect> pfx {};
  std::vector<MdsParticleEffectStop> pfx_stop {};
  std::vector<MdsSoundEffect> sfx {};
  std::vector<MdsSoundEffectGround> sfx_ground {};
  std::vector<MdsMorphAnimation> morph {};
  std::vector<MdsCameraTremor> tremors {};
  };

struct MdsAnimationAlias {
  std::string name;
  uint32_t layer;
  std::string next;
  float blend_in;
  float blend_out;
  AnimationFlags flags {AnimationFlags::NONE};
  std::string alias;
  AnimationDirection direction;
  };

struct MdsAnimationBlend {
  std::string name;
  std::string next;
  float blend_in {0};
  float blend_out {0};
  };

struct MdsAnimationCombine {
  std::string name;
  uint32_t layer;
  std::string next;
  float blend_in;
  float blend_out;
  AnimationFlags flags {AnimationFlags::NONE};
  std::string model;
  int32_t last_frame;
  };

struct ModelScript {
  MdsSkeleton skeleton {};
  std::vector<std::string> meshes {};
  std::vector<std::string> disabled_animations {};
  std::vector<MdsAnimationCombine> combinations {};
  std::vector<MdsAnimationBlend> blends {};
  std::vector<MdsAnimationAlias> aliases {};
  std::vector<MdsModelTag> model_tags {};
  std::vector<MdsAnimation> animations {};
  };

struct MorphAnimation {
  std::string name;
  int32_t layer;
  float blend_in;
  float blend_out;
  float duration;
  float speed;
  uint8_t flags;
  uint32_t frame_count;
  std::vector<uint32_t> vertices;
  std::vector<zenkit::Vec3> samples;
  };

struct MorphSource {
  ModelDate file_date;
  std::string file_name;
  };

struct MorphMesh {
  std::string name {};
  StaticMesh mesh {};
  std::vector<zenkit::Vec3> morph_positions {};
  std::vector<MorphAnimation> animations {};
  std::vector<MorphSource> sources {};
  };

struct Model {
  ModelHierarchy hierarchy {};
  ModelMesh mesh {};
  };

}
