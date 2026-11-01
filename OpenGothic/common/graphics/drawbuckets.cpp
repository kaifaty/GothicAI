#include "drawbuckets.h"

#include <Tempest/Vec>
#include <algorithm>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/shaders.h"
#include "frameprofile.h"

using namespace Tempest;

static size_t roundup(size_t i) {
  const size_t rounding = 1024;
  if(i%rounding!=0)
    i += rounding-i%rounding;
  return i;
  };

DrawBuckets::Id::Id(DrawBuckets* owner, size_t id):owner(owner), id(uint16_t(id)) {
  }

DrawBuckets::Id::Id(Id&& other) noexcept {
  std::swap(owner, other.owner);
  std::swap(id,    other.id);
  }

DrawBuckets::Id& DrawBuckets::Id::operator =(Id&& other) noexcept {
  std::swap(owner, other.owner);
  std::swap(id,    other.id);
  return *this;
  }

DrawBuckets::Id::~Id() {
  }


DrawBuckets::DrawBuckets():textureDesc(Resources::MaxFramesInFlight) {
  }

DrawBuckets::Id DrawBuckets::alloc(const Material& mat, const StaticMesh& mesh) {
  for(size_t i=0; i<bucketsCpu.size(); ++i) {
    auto& b = bucketsCpu[i];
    if(b.staticMesh==&mesh && b.mat==mat)
      return Id(this, i);
    }

  Bucket bx;
  bx.staticMesh = &mesh;
  bx.mat        = mat;

  bucketsDurtyBit = true;
  for(size_t i=0;i<bucketsCpu.size();++i)
    if(bucketsCpu[i].staticMesh==nullptr && bucketsCpu[i].animMesh==nullptr) {
      bucketsCpu[i] = std::move(bx);
      return Id(this,i);
      }
  bucketsCpu.emplace_back(std::move(bx));
  return Id(this, bucketsCpu.size()-1);
  }

DrawBuckets::Id DrawBuckets::alloc(const Material& mat, const AnimMesh& mesh) {
  for(size_t i=0; i<bucketsCpu.size(); ++i) {
    auto& b = bucketsCpu[i];
    if(b.animMesh==&mesh && b.mat==mat)
      return Id(this, i);
    }

  Bucket bx;
  bx.animMesh = &mesh;
  bx.mat      = mat;

  bucketsDurtyBit = true;
  bucketsCpu.emplace_back(std::move(bx));
  return Id(this, bucketsCpu.size()-1);
  }

void DrawBuckets::invalidate(const StaticMesh& mesh) {
  for(auto& bucket:bucketsCpu)
    if(bucket.staticMesh==&mesh) {
      bucket = Bucket();
      bucketsDurtyBit = true;
      }
  }

const std::vector<DrawBuckets::Bucket>& DrawBuckets::buckets() const {
  return bucketsCpu;
  }

void DrawBuckets::updateTextureArrays(uint8_t fId) {
  auto& bank=textureDesc[fId];
  for(size_t i=0;i<textureSlots.size();++i) {
    Resources::recycle(std::move(bank.arrays[i]));
    if(!textureSlots[i].empty())
      bank.arrays[i]=Resources::device().descriptors(textureSlots[i]);
    }
  bank.pending.clear();
  bank.rebuild=false;
  }

void DrawBuckets::updateBindlessArrays(bool updateBuffers, uint8_t fId) {
  if(!Shaders::options().doBindless)
    return;
  if(!updateBuffers) {
    FrameProfile::Scope timing(FrameProfile::Zone::BindlessPatch);
    if(textureGeneration!=Resources::textureGeneration()) {
      for(const auto* texture:Resources::textureChanges(textureGeneration)) {
        const auto found=textureUsers.find(texture);
        if(found==textureUsers.end())
          continue;
        for(auto& bank:textureDesc)
          if(!bank.rebuild)
            bank.pending.insert(bank.pending.end(),found->second.begin(),found->second.end());
        }
      }
    auto& bank=textureDesc[fId];
    if(bank.rebuild) {
      timing.finish();
      FrameProfile::Scope rebuildTiming(FrameProfile::Zone::BindlessRebuild);
      updateTextureArrays(fId);
      rebuildTiming.add(FrameProfile::Amount::BindlessTextureSlots,
                        textureSlots[0].size()+textureSlots[1].size()+textureSlots[2].size());
      return;
      }
    std::sort(bank.pending.begin(),bank.pending.end());
    bank.pending.erase(std::unique(bank.pending.begin(),bank.pending.end()),bank.pending.end());
    // commit runs after this frame slot's fence. Other banks retain their
    // previous descriptors until their own fence completes and they are used.
    for(const auto& [array,index]:bank.pending)
      bank.arrays[array].set(index,textureSlots[array][index]);
    timing.add(FrameProfile::Amount::BindlessPatchedSlots,bank.pending.size());
    bank.pending.clear();
    return;
    }
  FrameProfile::Scope timing(FrameProfile::Zone::BindlessRebuild);

  for(auto& slots:textureSlots)
    slots.clear();
  auto& tex=textureSlots[0];
  auto& normal=textureSlots[1];
  auto& orm=textureSlots[2];
  std::vector<const Tempest::StorageBuffer*> vbo, ibo;
  std::vector<const Tempest::StorageBuffer*> morphId, morph;
  for(auto& i:bucketsCpu) {
    tex.push_back(i.mat.tex);
    if(Shaders::options().doMaterialMaps) {
      normal.push_back(i.mat.normal!=nullptr ? i.mat.normal : &Resources::fallbackTexture());
      orm.push_back(i.mat.orm!=nullptr ? i.mat.orm : &Resources::fallbackTexture());
      }
    if(i.staticMesh!=nullptr) {
      ibo    .push_back(&i.staticMesh->ibo8Buffer());
      vbo    .push_back(&i.staticMesh->vboBuffer());
      morphId.push_back(i.staticMesh->morph.index);
      morph  .push_back(i.staticMesh->morph.samples);
      } else if(i.animMesh!=nullptr) {
      ibo    .push_back(&i.animMesh->ibo8);
      vbo    .push_back(&i.animMesh->vbo);
      morphId.push_back(nullptr);
      morph  .push_back(nullptr);
      } else {
      ibo    .push_back(nullptr);
      vbo    .push_back(nullptr);
      morphId.push_back(nullptr);
      morph  .push_back(nullptr);
      }
    }

  // The first slots retain bucket IDs for RT and nonanimated materials.
  // Landscape animations select their frame in the fragment shader so chunks
  // with the same pipeline share one indirect command.
  for(const auto& bucket:bucketsCpu)
    if(bucket.mat.hasFrameAnimation())
      tex.insert(tex.end(),bucket.mat.frames.begin(),bucket.mat.frames.end());

  // avoid shader linking stutter in vulkan
  tex    .resize(roundup(tex.size()));
  normal .resize(roundup(normal.size()));
  orm    .resize(roundup(orm.size()));

  textureUsers.clear();
  for(uint8_t array=0;array<textureSlots.size();++array)
    for(uint32_t i=0;i<textureSlots[array].size();++i)
      if(const auto* texture=textureSlots[array][i])
        textureUsers[texture].emplace_back(array,i);
  for(auto& bank:textureDesc) {
    bank.pending.clear();
    bank.rebuild=true;
    }
  updateTextureArrays(fId);

  auto& device = Resources::device();
  vbo    .resize(roundup(vbo.size()));
  ibo    .resize(roundup(ibo.size()));
  morphId.resize(roundup(morphId.size()));
  morph  .resize(roundup(morph.size()));
  Resources::recycle(std::move(desc.vbo));
  Resources::recycle(std::move(desc.ibo));
  Resources::recycle(std::move(desc.morphId));
  Resources::recycle(std::move(desc.morph));
  desc.vbo     = device.descriptors(vbo);
  desc.ibo     = device.descriptors(ibo);
  desc.morphId = device.descriptors(morphId);
  desc.morph   = device.descriptors(morph);
  timing.add(FrameProfile::Amount::BindlessTextureSlots,tex.size()+normal.size()+orm.size());
  timing.add(FrameProfile::Amount::BindlessBufferSlots,vbo.size()+ibo.size()+morphId.size()+morph.size());
  }

bool DrawBuckets::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  currentFrame=fId;
  const auto generation=Resources::textureGeneration();
  if(!bucketsDurtyBit) {
    const auto& bank=textureDesc[fId];
    if(textureGeneration==generation &&
       (!Shaders::options().doBindless || (!bank.rebuild && bank.pending.empty())))
      return false;
    updateBindlessArrays(false,fId);
    textureGeneration=generation;
    return true;
    }
  bucketsDurtyBit = false;
  textureGeneration=generation;

  std::vector<BucketGpu> bucket;
  uint32_t diffuseFrames=uint32_t(bucketsCpu.size());
  for(auto& i:bucketsCpu) {
    BucketGpu bx;
    if(i.mat.hasFrameAnimation()) {
      bx.diffuseFrames=diffuseFrames;
      bx.frameCount=uint32_t(i.mat.frames.size());
      bx.frameDuration=uint32_t(i.mat.texAniFPSInv);
      diffuseFrames+=bx.frameCount;
      }
    bx.texAniMapDirPeriod = i.mat.texAniMapDirPeriod;
    bx.waveMaxAmplitude   = i.mat.waveMaxAmplitude;
    bx.alphaWeight        = i.mat.alphaWeight;
    bx.envMapping         = i.mat.envMapping;
    if(i.staticMesh!=nullptr) {
      auto& bbox    = i.staticMesh->bbox.bbox;
      bx.bboxRadius = i.staticMesh->bbox.rConservative;
      bx.bbox[0]    = Vec4(bbox[0].x,bbox[0].y,bbox[0].z,0.f);
      bx.bbox[1]    = Vec4(bbox[1].x,bbox[1].y,bbox[1].z,0.f);
      if(i.staticMesh->morph.anim!=nullptr)
        bx.flags |= BK_MORPH;
      else if(i.mat.alpha==Material::Solid || i.mat.alpha==Material::AlphaTest) {
        bx.uvScale = i.mat.uvScale;
        if(i.mat.normal!=nullptr)
          bx.flags |= BK_NORMAL;
        if(i.mat.orm!=nullptr)
          bx.flags |= BK_ORM;
        if(i.mat.hasMaterialMaps())
          bx.flags |= BK_MATERIAL_MAPS;
        if(i.mat.hasMaterialMaps() && i.mat.antitile!=Material::Antitile::Off) {
          bx.antitileCell = i.mat.antitileCell;
          bx.flags |= BK_ANTITILE;
          if(i.mat.antitile==Material::Antitile::Rotate)
            bx.flags |= BK_ANTITILE_ROTATE;
          }
        }
      }
    else if(i.animMesh!=nullptr) {
      auto& bbox    = i.animMesh->bbox.bbox;
      bx.bboxRadius = i.animMesh->bbox.rConservative;
      bx.bbox[0]    = Vec4(bbox[0].x,bbox[0].y,bbox[0].z,0.f);
      bx.bbox[1]    = Vec4(bbox[1].x,bbox[1].y,bbox[1].z,0.f);
      bx.flags     |= BK_SKIN;
      }
    if(i.mat.alpha==Material::Solid)
      bx.flags |= BK_SOLID;
    if(i.mat.alpha==Material::Water)
      bx.flags |= BK_WATER;
    bucket.push_back(bx);
    }

  auto& device = Resources::device();
  Resources::recycle(std::move(bucketsGpu));
  bucketsGpu = device.ssbo(Uninitialized,bucket.size()*sizeof(bucket[0]));
  bucketsGpu.updateAsync(bucket.data(),0,bucket.size()*sizeof(bucket[0]));

  updateBindlessArrays(true,fId);
  return true;
  }
