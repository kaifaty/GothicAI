#include "drawbuckets.h"

#include <Tempest/Vec>
#include <algorithm>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/shaders.h"
#include "frameprofile.h"
#include "commandline.h"
#include "graphics/crownlight.h"
#include "graphics/eyeshader.h"
#include "graphics/foliagetree.h"
#include "graphics/hizoccluders.h"

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
      // the box the culling tests holds every level of detail of the mesh (meshlevels.h); its own is the wind's
      auto& bbox    = i.staticMesh->bbox.bbox;
      auto& drawn   = i.staticMesh->drawn.bbox;
      bx.bboxRadius = i.staticMesh->drawn.rConservative;
      bx.bbox[0]    = Vec4(drawn[0].x,drawn[0].y,drawn[0].z,0.f);
      bx.bbox[1]    = Vec4(drawn[1].x,drawn[1].y,drawn[1].z,0.f);
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
    bucket.push_back(bx);
    }

  auto& device = Resources::device();
  Resources::recycle(std::move(bucketsGpu));
  bucketsGpu = device.ssbo(Uninitialized,bucket.size()*sizeof(bucket[0]));
  bucketsGpu.updateAsync(bucket.data(),0,bucket.size()*sizeof(bucket[0]));

  updateBindlessArrays(true,fId);
  return true;
  }
