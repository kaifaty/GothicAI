#include "drawbuckets.h"

#include <Tempest/Vec>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <unordered_set>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/meshlevels.h"
#include "graphics/shaders.h"
#include "frameprofile.h"
#include "commandline.h"
#include "graphics/crownlight.h"
#include "graphics/eyeshader.h"
#include "graphics/foliagetree.h"
#include "graphics/hizoccluders.h"
#include "graphics/treeboughasset.h"

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

const Tempest::StorageBuffer& DrawBuckets::Bucket::iboBuffer() const {
  if(boughPage!=nullptr)
    return boughPage->ibo;
  return staticMesh!=nullptr ? staticMesh->ibo8Buffer() : animMesh->ibo8;
  }

const Tempest::StorageBuffer& DrawBuckets::Bucket::vboBuffer() const {
  if(boughPage!=nullptr)
    return boughPage->vbo;
  return staticMesh!=nullptr ? staticMesh->vboBuffer() : animMesh->vbo;
  }

void DrawBuckets::publishBuckets() {
  FrameProfile::Scope timing(FrameProfile::Zone::BucketPublish);
  const auto generation = Resources::textureGeneration();
  FrameProfile::add(FrameProfile::Amount::BucketsTotal,bucketsCpu.size());
  if(pubBuckets!=nullptr && !cpuDirty && pubGeneration==generation && pubBuckets->size()==bucketsCpu.size()) {
    curBuckets = pubBuckets.get();
    return;
    }
  static const bool selective = [] {
    const char* env = std::getenv("OPENGOTHIC_BUCKET_SELECTIVE_REFRESH");
    return env==nullptr || env[0]!='0';
    }();
  if(selective && pubBuckets!=nullptr && !cpuDirty && pubGeneration!=generation && pubBuckets->size()==bucketsCpu.size()) {
    const auto changes = Resources::textureChanges(pubGeneration);
    const std::unordered_set<const Texture2d*> changed(changes.begin(),changes.end());
    size_t refreshed = 0;
    for(size_t i=0; i<bucketsCpu.size(); ++i) {
      const auto& b = bucketsCpu[i];
      auto& pb = (*pubBuckets)[i];
      assert(pb.frames.size()==b.mat.frames.size());
      bool bucketChanged = false;
      auto refresh = [&](const Texture2d* source, Texture2d& destination) {
        if(source!=nullptr && changed.find(source)!=changed.end()) {
          destination = source->shared();
          bucketChanged = true;
          }
        };
      refresh(b.mat.tex,pb.tex);
      refresh(b.mat.normal,pb.normal);
      refresh(b.mat.orm,pb.orm);
      if(b.mat.hasFringe()) {
        refresh(b.mat.fringe.albedo,pb.frAlbedo);
        refresh(b.mat.fringe.normal,pb.frNormal);
        refresh(b.mat.fringe.orm,pb.frOrm);
        }
      for(size_t frame=0; frame<b.mat.frames.size(); ++frame)
        refresh(b.mat.frames[frame],pb.frames[frame]);
      refreshed += bucketChanged ? 1u : 0u;
      }
    // prepareFrame runs after joinRecord: no recording thread is reading this vector during a texture-only refresh.
    pubGeneration = generation;
    curBuckets = pubBuckets.get();
    FrameProfile::add(FrameProfile::Amount::BucketsRefreshed,refreshed);
    return;
    }
  pubGeneration = generation;
  auto p = std::make_shared<std::vector<BucketPub>>();
  p->reserve(bucketsCpu.size());
  for(const auto& b:bucketsCpu) {
    BucketPub pb;
    // the mesh buffers by value: Bucket::iboBuffer/vboBuffer resolve the bough page, the static mesh (or its
    // landscape page) or the anim mesh; the copied handles keep the allocation alive past a streaming teardown
    if(b.staticMesh!=nullptr || b.animMesh!=nullptr) {
      pb.ibo = b.iboBuffer();
      pb.vbo = b.vboBuffer();
      }
    if(b.staticMesh!=nullptr) {
      if(b.staticMesh->morph.index!=nullptr)
        pb.morphId = *b.staticMesh->morph.index;
      if(b.staticMesh->morph.samples!=nullptr)
        pb.morph = *b.staticMesh->morph.samples;
      }
    if(b.mat.tex!=nullptr)
      pb.tex = b.mat.tex->shared();
    if(b.mat.normal!=nullptr)
      pb.normal = b.mat.normal->shared();
    if(b.mat.orm!=nullptr)
      pb.orm = b.mat.orm->shared();
    if(b.mat.hasFringe()) {
      pb.frAlbedo = b.mat.fringe.albedo->shared();
      pb.frNormal = b.mat.fringe.normal->shared();
      pb.frOrm    = b.mat.fringe.orm->shared();
      }
    pb.fringeOn = b.mat.hasFringe();
    pb.frames.reserve(b.mat.frames.size());
    for(const auto* t:b.mat.frames)
      pb.frames.push_back(t->shared());
    pb.texAniFPSInv = b.mat.texAniFPSInv;
    pb.frameAni     = b.mat.hasFrameAnimation();
    p->push_back(std::move(pb));
    }
  pubBuckets = std::move(p);
  curBuckets = pubBuckets.get();
  cpuDirty   = false;
  FrameProfile::add(FrameProfile::Amount::BucketsRefreshed,pubBuckets->size());
  }

DrawBuckets::Id DrawBuckets::alloc(const Material& mat, const StaticMesh& mesh, const TreeBough::Page* page) {
  for(size_t i=0; i<bucketsCpu.size(); ++i) {
    auto& b = bucketsCpu[i];
    if(b.staticMesh==&mesh && b.boughPage==page && b.mat==mat)
      return Id(this, i);
    }

  Bucket bx;
  bx.staticMesh = &mesh;
  bx.boughPage  = page;
  bx.mat        = mat;

  bucketsDurtyBit = true;
  for(size_t i=0;i<bucketsCpu.size();++i)
    if(bucketsCpu[i].staticMesh==nullptr && bucketsCpu[i].animMesh==nullptr) {
      bucketsCpu[i] = std::move(bx);
      cpuDirty = true;
      return Id(this,i);
      }
  bucketsCpu.emplace_back(std::move(bx));
  cpuDirty = true;
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
  cpuDirty = true;
  return Id(this, bucketsCpu.size()-1);
  }

void DrawBuckets::invalidate(const StaticMesh& mesh) {
  spheres.forget(mesh);
  for(auto& bucket:bucketsCpu)
    if(bucket.staticMesh==&mesh) {
      bucket = Bucket();
      bucketsDurtyBit = true;
      cpuDirty = true;
      }
  }

const std::vector<DrawBuckets::Bucket>& DrawBuckets::buckets() const {
  return bucketsCpu;
  }

const DescriptorArray& DrawBuckets::textures() const { return textureDesc[currentFrame].arrays[0]; }
const DescriptorArray& DrawBuckets::normals() const  { return textureDesc[currentFrame].arrays[1]; }
const DescriptorArray& DrawBuckets::orm() const      { return textureDesc[currentFrame].arrays[2]; }

const DescriptorArray& DrawBuckets::vbo() const {
  const auto& bank = textureDesc[currentFrame];
  return bank.updateBuffers ? bank.buffers.vbo : desc.vbo;
  }

const DescriptorArray& DrawBuckets::ibo() const {
  const auto& bank = textureDesc[currentFrame];
  return bank.updateBuffers ? bank.buffers.ibo : desc.ibo;
  }

const DescriptorArray& DrawBuckets::morphId() const {
  const auto& bank = textureDesc[currentFrame];
  return bank.updateBuffers ? bank.buffers.morphId : desc.morphId;
  }

const DescriptorArray& DrawBuckets::morph() const {
  const auto& bank = textureDesc[currentFrame];
  return bank.updateBuffers ? bank.buffers.morph : desc.morph;
  }

template<class T>
static void publishArray(DescriptorArray& array, const std::vector<const T*>& slots, FrameProfile::Scope& timing,
                         FrameProfile::Amount allocated) {
  auto& device = Resources::device();
  if(const auto count = device.updateDescriptors(array, slots)) {
    timing.add(FrameProfile::Amount::BindlessPatchedSlots,*count);
    return;
    }
  Resources::recycle(std::move(array));
  array = device.descriptors(slots);
  device.updateDescriptors(array,slots,true);
  timing.add(allocated,slots.size());
  }

void DrawBuckets::updateTextureArrays(uint8_t fId, FrameProfile::Scope& timing) {
  auto& bank=textureDesc[fId];
  for(size_t i=0;i<textureSlots.size();++i) {
    if(textureSlots[i].empty())
      Resources::recycle(std::move(bank.arrays[i]));
    else
      publishArray(bank.arrays[i],textureSlots[i],timing,FrameProfile::Amount::BindlessTextureSlots);
    }
  bank.updateBuffers = bank.arrays[0].supportsBatchUpdates();
  bank.pending.clear();
  bank.rebuild=false;
  rebuiltTextureBank = true;
  }

void DrawBuckets::updateBufferArrays(uint8_t fId, FrameProfile::Scope& timing) {
  std::vector<const StorageBuffer*> vbo, ibo, morphId, morph;
  const auto capacity = roundup(bucketsCpu.size());
  for(auto* slots : {&vbo,&ibo,&morphId,&morph})
    slots->reserve(capacity);
  for(const auto& bucket : bucketsCpu) {
    if(bucket.staticMesh!=nullptr) {
      ibo    .push_back(&bucket.iboBuffer());
      vbo    .push_back(&bucket.vboBuffer());
      morphId.push_back(bucket.staticMesh->morph.index);
      morph  .push_back(bucket.staticMesh->morph.samples);
      } else if(bucket.animMesh!=nullptr) {
      ibo    .push_back(&bucket.animMesh->ibo8);
      vbo    .push_back(&bucket.animMesh->vbo);
      morphId.push_back(nullptr);
      morph  .push_back(nullptr);
      } else {
      ibo    .push_back(nullptr);
      vbo    .push_back(nullptr);
      morphId.push_back(nullptr);
      morph  .push_back(nullptr);
      }
    }
  for(auto* slots : {&vbo,&ibo,&morphId,&morph})
    slots->resize(capacity);

  auto& bank = textureDesc[fId];
  auto& dst  = bank.updateBuffers ? bank.buffers : desc;
  publishArray(dst.vbo,    vbo,    timing,FrameProfile::Amount::BindlessBufferSlots);
  publishArray(dst.ibo,    ibo,    timing,FrameProfile::Amount::BindlessBufferSlots);
  publishArray(dst.morphId,morphId,timing,FrameProfile::Amount::BindlessBufferSlots);
  publishArray(dst.morph,  morph,  timing,FrameProfile::Amount::BindlessBufferSlots);
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
      updateTextureArrays(fId,rebuildTiming);
      if(bank.updateBuffers)
        updateBufferArrays(fId,rebuildTiming);
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
  for(auto& i:bucketsCpu) {
    tex.push_back(i.mat.tex);
    if(Shaders::options().doMaterialMaps) {
      normal.push_back(i.mat.normal!=nullptr ? i.mat.normal : &Resources::fallbackTexture());
      orm.push_back(i.mat.orm!=nullptr ? i.mat.orm : &Resources::fallbackTexture());
      }
    }

  // The first slots retain bucket IDs for RT and nonanimated materials.
  // Landscape animations select their frame in the fragment shader so chunks
  // with the same pipeline share one indirect command.
  for(const auto& bucket:bucketsCpu)
    if(bucket.mat.hasFrameAnimation())
      tex.insert(tex.end(),bucket.mat.frames.begin(),bucket.mat.frames.end());
  // The fringes' maps follow at one index in all three arrays (commit gave the buckets their fringeTex)
  if(Shaders::options().doMaterialMaps && !fringeMaps.empty()) {
    normal.resize(tex.size());
    orm   .resize(tex.size());
    for(const auto& f:fringeMaps) {
      tex   .push_back(f[0]);
      normal.push_back(f[1]);
      orm   .push_back(f[2]);
      }
    }

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
  updateTextureArrays(fId,timing);
  updateBufferArrays(fId,timing);
  }

void DrawBuckets::updatePrimitiveCounts(Encoder<CommandBuffer>& cmd) {
  // One compact range per packed index buffer, shared by its materials. With per-material IBO bindings
  // (DrawCommands::drawVsm), the pair packer has no bindless array of all buckets to read.
  constexpr uint32_t none = uint32_t(-1);
  constexpr size_t meshletBytes = PackedMesh::MaxPrim*sizeof(uint32_t);
  std::unordered_map<const StorageBuffer*, uint32_t> buffers;
  std::vector<uint32_t> offsets(std::max<size_t>(1, bucketsCpu.size()), none);
  size_t total = 0;
  for(size_t i=0; i<bucketsCpu.size(); ++i) {
    const auto& bucket = bucketsCpu[i];
    // The compact cards of a bough only draw in the main and occluder views. The regular mesh casts the shadow.
    if((bucket.staticMesh==nullptr && bucket.animMesh==nullptr) || bucket.boughPage!=nullptr)
      continue;
    const auto& ibo = bucket.iboBuffer();
    if(ibo.isEmpty())
      continue;
    const auto [at, added] = buffers.try_emplace(&ibo, uint32_t(total));
    if(added)
      total += ibo.byteSize()/meshletBytes;
    offsets[i] = at->second;
    }

  auto& device = Resources::device();
  Resources::recycle(std::move(primitiveOffsets));
  Resources::recycle(std::move(primitiveCounts));
  primitiveOffsets = device.ssbo(offsets);
  primitiveCounts  = device.ssbo(Uninitialized, std::max<size_t>(1, total)*sizeof(uint32_t));
  cmd.setBinding(1, primitiveCounts);
  cmd.setPipeline(Shaders::inst().vsmCountPrimitives);
  for(const auto& [ibo, offset] : buffers) {
    const uint32_t push[] = {offset, PackedMesh::MaxPrim};
    cmd.setBinding(0, *ibo);
    cmd.setPushData(push, sizeof(push));
    cmd.dispatchThreads(ibo->byteSize()/meshletBytes);
    }
  }

bool DrawBuckets::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  rebuiltTextureBank = false;
  currentFrame=fId;
  publishBuckets();
  spheres.commit(cmd,fId);
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
  std::vector<StaticMesh::TreeHiZBounds> treeBounds;
  uint32_t diffuseFrames=uint32_t(bucketsCpu.size());
  for(auto& i:bucketsCpu)
    if(i.mat.hasFrameAnimation())
      diffuseFrames+=uint32_t(i.mat.frames.size());
  const uint32_t fringeBase=diffuseFrames;
  diffuseFrames=uint32_t(bucketsCpu.size());
  fringeMaps.clear();
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
    bx.turbidity          = i.mat.turbidity;
    bx.turbidityDepth     = i.mat.turbidityDepth;
    bx.emissive           = i.mat.emissive;
    bx.reactive           = i.mat.taaReactive();
    bx.alphaWeight        = i.mat.alphaWeight;
    bx.envMapping         = i.mat.envMapping;
    if(i.staticMesh!=nullptr) {
      if(i.staticMesh->ownMesh)
        bx.flags |= BK_OWN_MESH;
      // the box the culling tests holds every level of detail of the mesh (meshlevels.h); its own is the wind's
      auto& bbox    = i.staticMesh->bbox.bbox;
      auto& drawn   = i.staticMesh->drawn.bbox;
      bx.bboxRadius = i.staticMesh->drawn.rConservative;
      bx.bbox[0]    = Vec4(drawn[0].x,drawn[0].y,drawn[0].z,0.f);
      bx.bbox[1]    = Vec4(drawn[1].x,drawn[1].y,drawn[1].z,0.f);
      // what the shadow pages draw (DrawCommands::drawVsm) takes its object's meshlets by their spheres
      // The hierarchy has already registered one world sphere per meshlet with DrawClusters.
      if(i.boughPage==nullptr && i.staticMesh->clusterLevels.empty() &&
         (i.mat.alpha==Material::Solid || i.mat.alpha==Material::AlphaTest))
        bx.meshletSpheres = spheres.place(*i.staticMesh);
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
        // Maps by world position (material_projection.glsl): the slot of the UV scale, which the projection does not
        // use, carries repeats per unit of the world (cm). Opaque only: an alpha-tested one is rejected (Material)
        if(const float k = i.mat.projectionUvScale(); k>0) {
          bx.uvScale = k;
          bx.flags  |= BK_PROJECTION;
          }
        // Grass along the edges over the projection: its maps after the buckets in the bindless arrays, in its own
        // slots without them (DrawCommands::setBindings)
        if(i.mat.hasFringe()) {
          const auto& f = i.mat.fringe;
          const std::array<const Tempest::Texture2d*,3> set = {f.albedo,f.normal,f.orm};
          auto at = std::find(fringeMaps.begin(),fringeMaps.end(),set);
          if(at==fringeMaps.end())
            at = fringeMaps.insert(fringeMaps.end(),set);
          bx.fringeTex     = fringeBase+uint32_t(at-fringeMaps.begin());
          bx.fringeScale   = i.mat.projectionTile/f.tile;
          bx.fringeTop0    = f.top[0];
          bx.fringeTop1    = f.top[1];
          bx.fringeBottom0 = f.bottom[0];
          bx.fringeBottom1 = f.bottom[1];
          bx.flags        |= BK_FRINGE;
          }
        // Living foliage: the wind in the vertex shader and the sun through the leaves. A half that is switched off
        // packs as zero; with both off the material is drawn as any other of our own
        if(i.mat.foliage.on && i.mat.hasMaterialMaps()) {
          const uint32_t bits = FoliageWind::pack(i.mat.foliage, FoliageWind::enabled());
          if((bits & 0xFFFFFFu)!=0) {
            // the box the culling tests is the one the leaves stay in (visibility_pass.comp); the shader takes the
            // arm of the mesh at rest
            const float reach = FoliageTree::reach(i.mat.foliage,bbox[0],bbox[1]);
            const float grow  = FoliageTree::travel(bits,reach);
            bx.foliage      = bits;
            bx.foliageReach = reach;
            bx.bbox[0]     -= Vec4(grow,grow,grow,0.f);
            bx.bbox[1]     += Vec4(grow,grow,grow,0.f);
            bx.flags       |= BK_FOLIAGE;
            }
          // a tree's crown (graphics/crownlight.h): with the foliage's light, whatever the material lets through
          if(CrownLight::lit(i.mat.foliage, FoliageWind::enabled(), CrownLight::enabled(), i.mat.emissive))
            bx.flags |= BK_CROWN;
          }
        // A ground that grows grass (graphics/grass/grassfield.h): its look under the blades and beyond them, in
        // the landscape's G-buffer pass. The record excludes `foliage`, so the slot of its bits is free
        if(i.mat.grass.on && Shaders::options().doMaterialMaps && Grass::enabled().mode!=Grass::Off) {
          bx.foliage = Grass::pack(i.mat.grass);
          bx.flags  |= BK_GRASS;
          }
        // Its own two layers by world position, and the strip of them on the ground beside it (shader/grass/
        // grass_layers.glsl): their maps after the buckets in the bindless arrays, as a fringe's - the records
        // exclude `projection`, so the fringe's slots are free. Bindless only: without it the ground is as before.
        // A meadow's `ground.border` is the third triple; a road's `track` (shader/grass/grass_track.glsl) adds
        // its crust, loose earth and ribbon after the verge's two, or stands alone
        const bool layered = i.mat.layers.present() || i.mat.layers.hasTrack();
        if(layered && Shaders::options().doMaterialMaps && Shaders::options().doBindless &&
           Grass::enabled().mode!=Grass::Off && i.mat.projectionUvScale()<=0) {
          const auto& l      = i.mat.layers;
          const bool  ground = l.present(), track = l.hasTrack() && !i.mat.grass.on;
          std::vector<std::array<const Tempest::Texture2d*,3>> run;
          if(ground) {
            run.push_back({l.sward[0],l.sward[1],l.sward[2]});
            run.push_back({l.earth[0],l.earth[1],l.earth[2]});
            }
          if(ground && !l.verge.on && l.hasBorder())
            run.push_back(l.border);
          if(track) {
            run.push_back(l.crust);
            run.push_back(l.loose);
            run.push_back(l.ribbon);
            }
          size_t at = 0;
          while(at+run.size()<=fringeMaps.size() && !std::equal(run.begin(),run.end(),fringeMaps.begin()+ptrdiff_t(at)))
            ++at;
          if(at+run.size()>fringeMaps.size()) {
            at = fringeMaps.size();
            fringeMaps.insert(fringeMaps.end(),run.begin(),run.end());
            }
          bx.fringeTex = fringeBase+uint32_t(at);
          if(ground) {
            bx.fringeScale = 1.f/(l.ground.swardTile*100.f);
            bx.fringeTop0  = 1.f/(l.ground.earthTile*100.f);
            bx.fringeTop1  = l.ground.far;
            if(l.verge.on) {
              bx.foliage = l.grassBits;
              bx.flags  |= BK_VERGE;
              }
            else if(i.mat.grass.on) {
              bx.flags  |= BK_GROUND;
              if(l.hasBorder())
                bx.fringeBottom0 = 1.f/(l.borderTile*100.f);
              }
            }
          if(track) {
            bx.fringeBottom0 = 1.f/(l.track.crustTile*100.f);
            bx.fringeBottom1 = 1.f/(l.track.looseTile*100.f);
            bx.foliageReach  = Grass::pack(l.track);
            bx.flags        |= BK_TRACK;
            }
          }
        // A window lit of an evening (graphics/windowglow.h): its hours in the same slot; the record excludes
        // `grass`, and a window with leaves keeps the leaves, a verge its grass's numbers
        if(i.mat.window.on && i.mat.emissive>0.f && (bx.flags & (BK_FOLIAGE|BK_GRASS|BK_VERGE))==0) {
          bx.foliage = WindowGlow::pack(i.mat.window);
          bx.flags  |= BK_WINDOW;
          }
        // Both sides of an object's mesh: the pipelines do not cull it (Shaders::materialPipeline); the bits are for
        // the mesh shader of the shadow pages, which culls by itself, and for the normal of the other side
        if(i.mat.twoSided!=Material::TwoSided::Off) {
          bx.flags |= BK_TWO_SIDED;
          if(i.mat.twoSided==Material::TwoSided::Flip)
            bx.flags |= BK_TWO_SIDED_FLIP;
          }
        }
      }
    else if(i.animMesh!=nullptr) {
      auto& bbox    = i.animMesh->bbox.bbox;
      bx.bboxRadius = i.animMesh->bbox.rConservative;
      bx.bbox[0]    = Vec4(bbox[0].x,bbox[0].y,bbox[0].z,0.f);
      bx.bbox[1]    = Vec4(bbox[1].x,bbox[1].y,bbox[1].z,0.f);
      bx.flags     |= BK_SKIN;
      // own skins (materials.json) bring normal and ORM maps like static meshes; no antitile on a body
      if(i.mat.alpha==Material::Solid || i.mat.alpha==Material::AlphaTest) {
        bx.uvScale = i.mat.uvScale;
        if(i.mat.normal!=nullptr)
          bx.flags |= BK_NORMAL;
        if(i.mat.orm!=nullptr)
          bx.flags |= BK_ORM;
        if(i.mat.hasMaterialMaps())
          bx.flags |= BK_MATERIAL_MAPS;
        }
      }
    if(i.mat.alpha==Material::Solid)
      bx.flags |= BK_SOLID;
    // A tree's alpha-tested leaves (graphics/hizoccluders.h): the occluder pass leaves them out while its switch is
    // on; the counters of the main view tell them by this either way
    if(i.staticMesh!=nullptr && HiZOccluders::canopy(i.mat.foliage, i.mat.alpha==Material::AlphaTest))
      bx.flags |= BK_CANOPY;
    // Our glTF MASK (brows, lashes, hair cards): under TAA its edge averages to the texel's coverage. The original's
    // alpha test (foliage) keeps the fixed threshold
    if(i.mat.alpha==Material::AlphaTest && i.mat.isAlphaTestRequired() && CommandLine::inst().doHashedAlpha())
      bx.flags |= BK_HASHED_ALPHA;
    // Hair cards: the G-buffer takes the strand's direction and the hair bit, the light passes the hair BRDF
    if(i.mat.hair && (bx.flags & BK_MATERIAL_MAPS)!=0)
      bx.flags |= BK_HAIR;
    // Strands: the vertex shader makes ribbons of them (graphics/mesh/groommesh.cpp)
    if(i.mat.strand)
      bx.flags |= BK_STRAND;
    // An eyeball: the wet highlight and the lid's shade (OPENGOTHIC_EYE_SHADER=0: lit as any own material)
    if(i.mat.eye && (bx.flags & BK_MATERIAL_MAPS)!=0 && eyeShaderEnabled())
      bx.flags |= BK_EYE;
    if(i.mat.alpha==Material::Water)
      bx.flags |= BK_WATER;
    if(i.staticMesh!=nullptr && i.boughPage==nullptr && (i.mat.alpha==Material::Solid || i.mat.alpha==Material::AlphaTest) &&
       (bx.flags & (BK_MORPH|BK_STRAND|BK_GRASS|BK_VERGE|BK_TRACK))==0 && FoliageTree::isTree(i.mat.foliage)) {
      const auto bounds = i.staticMesh->treeHiZBounds();
      bx.treeHiZFirst = uint32_t(treeBounds.size());
      bx.treeHiZCount = uint32_t(bounds.size());
      treeBounds.insert(treeBounds.end(),bounds.begin(),bounds.end());
      }
    bucket.push_back(bx);
    }

  // the spheres of the meshes placed above, in this frame's commands before its shadow pages
  spheres.commit(cmd,fId);

  auto& device = Resources::device();
  Resources::recycle(std::move(bucketsGpu));
  bucketsGpu = device.ssbo(Uninitialized,bucket.size()*sizeof(bucket[0]));
  bucketsGpu.updateAsync(bucket.data(),0,bucket.size()*sizeof(bucket[0]));
  if(treeBounds.empty())
    treeBounds.emplace_back(); // binding for views without trees
  Resources::recycle(std::move(treeHiZBoundsGpu));
  treeHiZBoundsGpu = device.ssbo(Uninitialized,treeBounds.size()*sizeof(treeBounds[0]));
  treeHiZBoundsGpu.updateAsync(treeBounds.data(),0,treeBounds.size()*sizeof(treeBounds[0]));

  updateBindlessArrays(true,fId);
  if(MeshLevels::vsmFrameStatsEnabled() && Shaders::isVsmSupported())
    updatePrimitiveCounts(cmd);
  return true;
  }
