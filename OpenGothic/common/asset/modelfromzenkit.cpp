#include "fromzenkit.h"

#include <zenkit/Model.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/Stream.hh>

#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace asset {

static_assert(std::is_standard_layout_v<AnimationSample>);
static_assert(std::is_standard_layout_v<zenkit::AnimationSample>);
static_assert(std::is_trivially_copyable_v<AnimationSample>);
static_assert(std::is_trivially_copyable_v<zenkit::AnimationSample>);
static_assert(sizeof(AnimationSample)==7*sizeof(float));
static_assert(sizeof(AnimationSample)==sizeof(zenkit::AnimationSample));
static_assert(alignof(AnimationSample)==alignof(zenkit::AnimationSample));
static_assert(offsetof(AnimationSample,position)==offsetof(zenkit::AnimationSample,position));
static_assert(offsetof(AnimationSample,rotation)==offsetof(zenkit::AnimationSample,rotation));
static_assert(std::is_same_v<decltype(AnimationSample::position),decltype(zenkit::AnimationSample::position)>);
static_assert(std::is_same_v<decltype(AnimationSample::rotation),decltype(zenkit::AnimationSample::rotation)>);
static_assert(offsetof(AnimationSample,position)==0);
static_assert(offsetof(AnimationSample,rotation)==3*sizeof(float));
static_assert(offsetof(zenkit::Quat,w)==0);
static_assert(offsetof(zenkit::Quat,x)==sizeof(float));
static_assert(offsetof(zenkit::Quat,y)==2*sizeof(float));
static_assert(offsetof(zenkit::Quat,z)==3*sizeof(float));
static_assert(AnimationEvent::VMAX==zenkit::AnimationEvent::VMAX);
static_assert(std::is_same_v<std::underlying_type_t<AnimationEventType>,std::underlying_type_t<zenkit::AnimationEventType>>);
static_assert(std::is_same_v<std::underlying_type_t<MdsEventType>,std::underlying_type_t<zenkit::MdsEventType>>);
static_assert(std::is_same_v<std::underlying_type_t<MdsFightMode>,std::underlying_type_t<zenkit::MdsFightMode>>);
static_assert(std::is_same_v<std::underlying_type_t<AnimationFlags>,std::underlying_type_t<zenkit::AnimationFlags>>);
static_assert(std::is_same_v<std::underlying_type_t<AnimationDirection>,std::underlying_type_t<zenkit::AnimationDirection>>);

static_assert(int(AnimationEventType::TAG)==int(zenkit::AnimationEventType::TAG));
static_assert(int(AnimationEventType::SOUND_EFFECT)==int(zenkit::AnimationEventType::SOUND_EFFECT));
static_assert(int(AnimationEventType::SOUND_EFFECT_GROUND)==int(zenkit::AnimationEventType::SOUND_EFFECT_GROUND));
static_assert(int(AnimationEventType::BATCH)==int(zenkit::AnimationEventType::BATCH));
static_assert(int(AnimationEventType::SWAP_MESH)==int(zenkit::AnimationEventType::SWAP_MESH));
static_assert(int(AnimationEventType::HEADING)==int(zenkit::AnimationEventType::HEADING));
static_assert(int(AnimationEventType::PARTICLE_EFFECT)==int(zenkit::AnimationEventType::PARTICLE_EFFECT));
static_assert(int(AnimationEventType::PARTICLE_EFFECT_GROUND)==int(zenkit::AnimationEventType::PARTICLE_EFFECT_GROUND));
static_assert(int(AnimationEventType::PARTICLE_EFFECT_STOP)==int(zenkit::AnimationEventType::PARTICLE_EFFECT_STOP));
static_assert(int(AnimationEventType::SET_MESH)==int(zenkit::AnimationEventType::SET_MESH));
static_assert(int(AnimationEventType::MORPH_MESH_ANIMATION)==int(zenkit::AnimationEventType::MORPH_MESH_ANIMATION));
static_assert(int(AnimationEventType::CAMERA_TREMOR)==int(zenkit::AnimationEventType::CAMERA_TREMOR));
static_assert(int(MdsEventType::UNKNOWN)==int(zenkit::MdsEventType::UNKNOWN));
static_assert(int(MdsEventType::ITEM_CREATE)==int(zenkit::MdsEventType::ITEM_CREATE));
static_assert(int(MdsEventType::ITEM_INSERT)==int(zenkit::MdsEventType::ITEM_INSERT));
static_assert(int(MdsEventType::ITEM_REMOVE)==int(zenkit::MdsEventType::ITEM_REMOVE));
static_assert(int(MdsEventType::ITEM_DESTROY)==int(zenkit::MdsEventType::ITEM_DESTROY));
static_assert(int(MdsEventType::ITEM_PLACE)==int(zenkit::MdsEventType::ITEM_PLACE));
static_assert(int(MdsEventType::ITEM_EXCHANGE)==int(zenkit::MdsEventType::ITEM_EXCHANGE));
static_assert(int(MdsEventType::SET_FIGHT_MODE)==int(zenkit::MdsEventType::SET_FIGHT_MODE));
static_assert(int(MdsEventType::MUNITION_PLACE)==int(zenkit::MdsEventType::MUNITION_PLACE));
static_assert(int(MdsEventType::MUNITION_REMOVE)==int(zenkit::MdsEventType::MUNITION_REMOVE));
static_assert(int(MdsEventType::SOUND_DRAW)==int(zenkit::MdsEventType::SOUND_DRAW));
static_assert(int(MdsEventType::SOUND_UNDRAW)==int(zenkit::MdsEventType::SOUND_UNDRAW));
static_assert(int(MdsEventType::MESH_SWAP)==int(zenkit::MdsEventType::MESH_SWAP));
static_assert(int(MdsEventType::TORCH_DRAW)==int(zenkit::MdsEventType::TORCH_DRAW));
static_assert(int(MdsEventType::TORCH_INVENTORY)==int(zenkit::MdsEventType::TORCH_INVENTORY));
static_assert(int(MdsEventType::TORCH_DROP)==int(zenkit::MdsEventType::TORCH_DROP));
static_assert(int(MdsEventType::HIT_LIMB)==int(zenkit::MdsEventType::HIT_LIMB));
static_assert(int(MdsEventType::HIT_DIRECTION)==int(zenkit::MdsEventType::HIT_DIRECTION));
static_assert(int(MdsEventType::DAMAGE_MULTIPLIER)==int(zenkit::MdsEventType::DAMAGE_MULTIPLIER));
static_assert(int(MdsEventType::PARRY_FRAME)==int(zenkit::MdsEventType::PARRY_FRAME));
static_assert(int(MdsEventType::OPTIMAL_FRAME)==int(zenkit::MdsEventType::OPTIMAL_FRAME));
static_assert(int(MdsEventType::HIT_END)==int(zenkit::MdsEventType::HIT_END));
static_assert(int(MdsEventType::COMBO_WINDOW)==int(zenkit::MdsEventType::COMBO_WINDOW));
static_assert(int(MdsFightMode::FIST)==int(zenkit::MdsFightMode::FIST));
static_assert(int(MdsFightMode::SINGLE_HANDED)==int(zenkit::MdsFightMode::SINGLE_HANDED));
static_assert(int(MdsFightMode::DUAL_HANDED)==int(zenkit::MdsFightMode::DUAL_HANDED));
static_assert(int(MdsFightMode::BOW)==int(zenkit::MdsFightMode::BOW));
static_assert(int(MdsFightMode::CROSSBOW)==int(zenkit::MdsFightMode::CROSSBOW));
static_assert(int(MdsFightMode::MAGIC)==int(zenkit::MdsFightMode::MAGIC));
static_assert(int(MdsFightMode::NONE)==int(zenkit::MdsFightMode::NONE));
static_assert(int(MdsFightMode::INVALID)==int(zenkit::MdsFightMode::INVALID));
static_assert(int(AnimationFlags::NONE)==int(zenkit::AnimationFlags::NONE));
static_assert(int(AnimationFlags::MOVE)==int(zenkit::AnimationFlags::MOVE));
static_assert(int(AnimationFlags::ROTATE)==int(zenkit::AnimationFlags::ROTATE));
static_assert(int(AnimationFlags::QUEUE)==int(zenkit::AnimationFlags::QUEUE));
static_assert(int(AnimationFlags::FLY)==int(zenkit::AnimationFlags::FLY));
static_assert(int(AnimationFlags::IDLE)==int(zenkit::AnimationFlags::IDLE));
static_assert(int(AnimationFlags::INPLACE)==int(zenkit::AnimationFlags::INPLACE));
static_assert(int(AnimationDirection::FORWARD)==int(zenkit::AnimationDirection::FORWARD));
static_assert(int(AnimationDirection::BACKWARD)==int(zenkit::AnimationDirection::BACKWARD));

static ModelDate modelDate(const zenkit::Date& s) {
  return {s.year,s.month,s.day,s.hour,s.minute,s.second};
  }

static MdsEventTag eventTag(const zenkit::MdsEventTag& s) {
  return {s.frame,static_cast<MdsEventType>(s.type),s.slot,s.slot2,s.item,s.frames,
          static_cast<MdsFightMode>(s.fight_mode),s.attached};
  }

static MdsAnimation animation(const zenkit::MdsAnimation& s) {
  MdsAnimation m;
  m.name=s.name;
  m.layer=s.layer;
  m.next=s.next;
  m.blend_in=s.blend_in;
  m.blend_out=s.blend_out;
  m.flags=static_cast<AnimationFlags>(s.flags);
  m.model=s.model;
  m.direction=static_cast<AnimationDirection>(s.direction);
  m.first_frame=s.first_frame;
  m.last_frame=s.last_frame;
  m.fps=s.fps;
  m.speed=s.speed;
  m.collision_volume_scale=s.collision_volume_scale;
  m.events.reserve(s.events.size());
  for(const auto& e:s.events)
    m.events.push_back(eventTag(e));
  m.pfx.reserve(s.pfx.size());
  for(const auto& p:s.pfx)
    m.pfx.push_back({p.frame,p.index,p.name,p.position,p.attached});
  m.pfx_stop.reserve(s.pfx_stop.size());
  for(const auto& p:s.pfx_stop)
    m.pfx_stop.push_back({p.frame,p.index});
  m.sfx.reserve(s.sfx.size());
  for(const auto& e:s.sfx)
    m.sfx.push_back({e.frame,e.name,e.range,e.empty_slot});
  m.sfx_ground.reserve(s.sfx_ground.size());
  for(const auto& e:s.sfx_ground)
    m.sfx_ground.push_back({e.frame,e.name,e.range,e.empty_slot});
  m.morph.reserve(s.morph.size());
  for(const auto& e:s.morph)
    m.morph.push_back({e.frame,e.animation,e.node});
  m.tremors.reserve(s.tremors.size());
  for(const auto& e:s.tremors)
    m.tremors.push_back({e.frame,e.field1,e.field2,e.field3,e.field4});
  return m;
  }

ModelHierarchy fromZenKit(const zenkit::ModelHierarchy& s) {
  ModelHierarchy m;
  m.nodes.reserve(s.nodes.size());
  for(const auto& n:s.nodes)
    m.nodes.push_back({n.parent_index,n.name,n.transform});
  m.bbox=s.bbox;
  m.collision_bbox=s.collision_bbox;
  m.root_translation=s.root_translation;
  m.checksum=s.checksum;
  m.source_date=modelDate(s.source_date);
  m.source_path=s.source_path;
  return m;
  }

ModelAnimation fromZenKit(const zenkit::ModelAnimation& s) {
  ModelAnimation m;
  m.name=s.name;
  m.next=s.next;
  m.layer=s.layer;
  m.frame_count=s.frame_count;
  m.node_count=s.node_count;
  m.fps=s.fps;
  m.fps_source=s.fps_source;
  m.sample_position_min=s.sample_position_min;
  m.sample_position_scale=s.sample_position_scale;
  m.bbox=s.bbox;
  m.checksum=s.checksum;
  m.source_path=s.source_path;
  m.source_date=modelDate(s.source_date);
  m.source_script=s.source_script;
  m.samples.reserve(s.samples.size());
  for(const auto& sample:s.samples)
    m.samples.push_back({sample.position,sample.rotation});
  m.events.reserve(s.events.size());
  for(const auto& e:s.events) {
    AnimationEvent event;
    event.type=static_cast<AnimationEventType>(e.type);
    event.frame=e.frame;
    event.tag=e.tag;
    for(size_t i=0;i<AnimationEvent::VMAX;++i) {
      event.content[i]=e.content[i];
      event.values[i]=e.values[i];
      }
    event.probability=e.probability;
    m.events.push_back(std::move(event));
    }
  m.node_indices=s.node_indices;
  return m;
  }

ModelScript fromZenKit(const zenkit::ModelScript& s) {
  ModelScript m;
  m.skeleton={s.skeleton.name,s.skeleton.disable_mesh};
  m.meshes=s.meshes;
  m.disabled_animations=s.disabled_animations;
  m.combinations.reserve(s.combinations.size());
  for(const auto& a:s.combinations)
    m.combinations.push_back({a.name,a.layer,a.next,a.blend_in,a.blend_out,
                              static_cast<AnimationFlags>(a.flags),a.model,a.last_frame});
  m.blends.reserve(s.blends.size());
  for(const auto& a:s.blends)
    m.blends.push_back({a.name,a.next,a.blend_in,a.blend_out});
  m.aliases.reserve(s.aliases.size());
  for(const auto& a:s.aliases)
    m.aliases.push_back({a.name,a.layer,a.next,a.blend_in,a.blend_out,
                        static_cast<AnimationFlags>(a.flags),a.alias,static_cast<AnimationDirection>(a.direction)});
  m.model_tags.reserve(s.model_tags.size());
  for(const auto& t:s.model_tags)
    m.model_tags.push_back({t.bone});
  m.animations.reserve(s.animations.size());
  for(const auto& a:s.animations)
    m.animations.push_back(animation(a));
  return m;
  }

MorphMesh fromZenKit(const zenkit::MorphMesh& s) {
  MorphMesh m;
  m.name=s.name;
  m.mesh=fromZenKit(s.mesh);
  m.morph_positions=s.morph_positions;
  m.animations.reserve(s.animations.size());
  for(const auto& a:s.animations)
    m.animations.push_back({a.name,a.layer,a.blend_in,a.blend_out,a.duration,a.speed,
                            a.flags,a.frame_count,a.vertices,a.samples});
  m.sources.reserve(s.sources.size());
  for(const auto& source:s.sources)
    m.sources.push_back({modelDate(source.file_date),source.file_name});
  return m;
  }

Model fromZenKit(const zenkit::Model& s) {
  return {fromZenKit(s.hierarchy),fromZenKit(s.mesh)};
  }

std::vector<QuantizedAnimationSample> animationSamplesFromZenKit(zenkit::Read* source,
                                                               const zenkit::ModelAnimation& animation) {
  const uint64_t count=uint64_t(animation.frame_count)*animation.node_count;
  if(count!=animation.samples.size())
    throw std::runtime_error("animation frame and node counts differ from decoded samples");
  const auto start=source->tell();
  source->seek(0,zenkit::Whence::END);
  const auto end=source->tell();
  source->seek(0,zenkit::Whence::BEG);
  std::vector<QuantizedAnimationSample> samples;
  bool found=false;
  while(source->tell()<end) {
    if(end-source->tell()<6)
      throw std::runtime_error("truncated animation chunk header");
    const auto type=source->read_ushort();
    const auto size=source->read_uint();
    const auto payload=source->tell();
    if(size>end-payload)
      throw std::runtime_error("animation chunk outside the source");
    if(type==0xA090) {
      if(found)
        throw std::runtime_error("multiple animation sample chunks are unsupported");
      const uint64_t prefix=4+uint64_t(animation.node_count)*4;
      if(prefix>size || count>(size-prefix)/12)
        throw std::runtime_error("animation samples outside the source chunk");
      source->seek(static_cast<std::ptrdiff_t>(prefix),zenkit::Whence::CUR);
      samples.resize(static_cast<size_t>(count));
      for(auto& sample:samples) {
        for(auto& component:sample.rotation) component=source->read_ushort();
        for(auto& component:sample.position) component=source->read_ushort();
        }
      found=true;
      }
    source->seek(static_cast<std::ptrdiff_t>(payload+size),zenkit::Whence::BEG);
    }
  if(!found && count!=0)
    throw std::runtime_error("animation sample chunk is missing");
  source->seek(static_cast<std::ptrdiff_t>(start),zenkit::Whence::BEG);
  return samples;
  }

std::vector<std::string> modelAttachmentOrder(zenkit::Read* source) {
  const auto start=source->tell();
  source->seek(0,zenkit::Whence::END);
  const auto end=source->tell();
  source->seek(static_cast<std::ptrdiff_t>(start),zenkit::Whence::BEG);
  std::vector<std::string> order;
  while(source->tell()<end) {
    if(end-source->tell()<6)
      throw std::runtime_error("truncated model-mesh chunk header");
    const auto type=source->read_ushort();
    const auto size=source->read_uint();
    const auto payload=source->tell();
    // Soft-skin chunks include trailing mesh sections outside their length.
    // Their appearance before NODES means that this model has no attachments.
    if(type==0xD030 || type==0xD040)
      break;
    if(size>end-payload)
      throw std::runtime_error("model-mesh chunk outside the source");
    if(type==0xD020) {
      if(size<2)
        throw std::runtime_error("truncated model attachment count");
      const auto count=source->read_ushort();
      order.reserve(count);
      for(uint16_t i=0;i<count;++i) {
        order.push_back(source->read_line(true));
        if(source->tell()>payload+size)
          throw std::runtime_error("model attachment names outside the source chunk");
        }
      break;
      }
    source->seek(static_cast<std::ptrdiff_t>(payload+size),zenkit::Whence::BEG);
    }
  source->seek(static_cast<std::ptrdiff_t>(start),zenkit::Whence::BEG);
  return order;
  }

}
