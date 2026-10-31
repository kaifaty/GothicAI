#include "material.h"

#include "utils/parser.h"
#include "resources.h"

using namespace Tempest;

static Tempest::Color toColor(zenkit::Color v) {
  Tempest::Color c(float(v.r)/255.f, float(v.g)/255.f, float(v.b)/255.f, float(v.a)/255.f);
  return c;
  }

Material::Material(const asset::Material& m, bool enableAlphaTest, bool requireAlphaTest)
  :alphaTestEnabled(enableAlphaTest), alphaTestRequired(enableAlphaTest && requireAlphaTest),
   sourceAlphaFunc(m.alpha_func),
   sourceMaterialGroup(m.group), sourceColorAlpha(m.color.a) {
  loadFrames(m);

  Resources::MaterialMaps maps;
  if(m.group!=asset::MaterialGroup::WATER && !hasFrameAnimation() &&
     m.wave_mode==asset::WaveMode::NONE &&
     (m.texture_anim_map_mode==asset::AnimationMapping::NONE ||
      (m.texture_anim_map_dir.x==0.f && m.texture_anim_map_dir.y==0.f))) {
    maps = Resources::loadMaterialMaps(m.texture);
    // Our colour also determines alpha classification; consulting the original
    // texture format here would open an asset that this record replaces.
    if(maps.hasAlbedo) {
      tex = Resources::loadMaterialAlbedo(m.texture);
      }
    }
  if(tex==nullptr)
    tex = Resources::loadTexture(m.texture);
  if(tex==nullptr) {
    if(!m.texture.empty()) {
      tex = Resources::loadTexture("DEFAULT.TGA");
      } else {
      tex = Resources::loadTexture(toColor(m.color));
      alphaTestEnabled &= (m.color.a!=255);
      }
    }

  alpha = loadAlphaFunc(sourceAlphaFunc,sourceMaterialGroup,sourceColorAlpha,tex,alphaTestEnabled,alphaTestRequired);
  classifiedAlpha = alpha;
  if(alpha==Water && m.name=="OWODWFALL_WATERFALL_01") {
    // NOTE: waterfall heuristics
    alpha = Solid;
    }

  if(m.texture_anim_map_mode!=asset::AnimationMapping::NONE && tex!=nullptr) {
    auto texAniMapDir = m.texture_anim_map_dir;
    if(texAniMapDir.x!=0.f)
      texAniMapDirPeriod.x = int(1.f/texAniMapDir.x);
    if(texAniMapDir.y!=0.f)
      texAniMapDirPeriod.y = int(1.f/texAniMapDir.y);
    }

  if(m.wave_mode!=asset::WaveMode::NONE)
    waveMaxAmplitude = m.wave_max_amplitude;

  if(m.environment_mapping!=0)
    ; // envMapping = m.environment_mapping_strength;

  if(isSolid() && m.group!=asset::MaterialGroup::WATER &&
     !hasFrameAnimation() && !hasUvAnimation() && m.wave_mode==asset::WaveMode::NONE) {
    if(!maps.present)
      maps = Resources::loadMaterialMaps(m.texture);
    normal  = maps.normal;
    orm     = maps.orm;
    uvScale = maps.scale;
    antitileCell = maps.antitileCell;
    antitile = maps.antitile;
    materialMaps = maps.present;
    }
  }

Material::Material(const asset::VisualDecal& decal)
  :alphaTestEnabled(true), sourceAlphaFunc(static_cast<asset::AlphaFunction>(decal.alpha_func)), sourceColorAlpha(decal.alpha_weight) {
  tex = Resources::loadTexture(decal.name);
  if(tex==nullptr && !decal.name.empty())
    tex = Resources::loadTexture("DEFAULT.TGA");
  loadFrames(decal.name, decal.texture_anim_fps);

  alpha        = loadAlphaFunc(sourceAlphaFunc, sourceMaterialGroup, sourceColorAlpha, tex, alphaTestEnabled);
  classifiedAlpha = alpha;
  alphaWeight  = float(decal.alpha_weight)/255.f;
  }

Material::Material(const Def::ParticleEffect& src) {
  tex = Resources::loadTexture(src.vis_name_s);
  loadFrames(src.vis_name_s, src.vis_tex_ani_fps);

  //TODO: visTexAniIsLooping
  alpha = Parser::loadAlpha(src.vis_alpha_func_s);
  }

bool Material::operator ==(const Material& other) const {
  return tex==other.tex &&
         normal==other.normal &&
         orm==other.orm &&
         uvScale==other.uvScale &&
         antitileCell==other.antitileCell &&
         antitile==other.antitile &&
         materialMaps==other.materialMaps &&
         frames==other.frames &&
         alpha==other.alpha &&
         alphaTestEnabled==other.alphaTestEnabled &&
         alphaTestRequired==other.alphaTestRequired &&
         sourceAlphaFunc==other.sourceAlphaFunc &&
         sourceMaterialGroup==other.sourceMaterialGroup &&
         sourceColorAlpha==other.sourceColorAlpha &&
         classifiedAlpha==other.classifiedAlpha &&
         alphaWeight==other.alphaWeight &&
         texAniMapDirPeriod==other.texAniMapDirPeriod &&
         texAniFPSInv==other.texAniFPSInv &&
         isGhost==other.isGhost &&
         waveMaxAmplitude==other.waveMaxAmplitude &&
         envMapping==other.envMapping;
  }

bool Material::isSolid() const {
  return !isGhost && (alpha==Material::Solid || alpha==Material::AlphaTest);
  }

void Material::setAlbedo(const Tempest::Texture2d* texture) {
  tex = texture;
  if(alpha==classifiedAlpha) {
    alpha = loadAlphaFunc(sourceAlphaFunc,sourceMaterialGroup,sourceColorAlpha,tex,alphaTestEnabled,alphaTestRequired);
    classifiedAlpha = alpha;
    }
  }

bool Material::isForwardShading(AlphaFunc alpha) {
  return alpha!=Material::Solid && alpha!=Material::AlphaTest && alpha!=Material::Ghost;
  }

bool Material::isSceneInfoRequired(AlphaFunc alpha) {
  return alpha==Material::Water || alpha==Material::Ghost;
  }

bool Material::isShadowmapRequired(AlphaFunc alpha) {
  return isForwardShading(alpha) &&
         alpha!=Material::AdditiveLight && alpha!=Material::Multiply && alpha!=Material::Multiply2 &&
         alpha!=Material::Water;
  }

bool Material::isTextureInShadowPass(AlphaFunc alpha) {
  return (alpha==Material::AlphaTest);
  }

bool Material::isTesselated(AlphaFunc alpha) {
  // if(alpha!=Material::Water)
  //   return false;
  // return waveMaxAmplitude!=0.f;
  return (alpha==Material::Water);
  }

Material::AlphaFunc Material::loadAlphaFunc(asset::AlphaFunction zenAlpha,
                                            asset::MaterialGroup matGroup,
                                            uint8_t clrAlpha,
                                            const Tempest::Texture2d* tex,
                                            bool enableAlphaTest, bool requireAlphaTest) {
  Material::AlphaFunc alpha = Material::AlphaFunc::AlphaTest;
  switch(zenAlpha) {
    case asset::AlphaFunction::BLEND:
      alpha = Material::AlphaFunc::Transparent;
      break;
    case asset::AlphaFunction::ADD:
      alpha = Material::AlphaFunc::AdditiveLight;
      break;
    case asset::AlphaFunction::SUBTRACT:
      // FIXME: no such materials in game found
      alpha = Material::AlphaFunc::AdditiveLight;
      break;
    case asset::AlphaFunction::MULTIPLY:
      alpha = Material::AlphaFunc::Multiply;
      break;
    case asset::AlphaFunction::MULTIPLY_ALT:
      alpha = Material::AlphaFunc::Multiply2;
      break;
    case asset::AlphaFunction::DEFAULT:
    case asset::AlphaFunction::NONE:
      alpha = Material::AlphaFunc::AlphaTest;
      break;
    }

  if(matGroup == asset::MaterialGroup::WATER)
    alpha = Material::AlphaFunc::Water;

  if(clrAlpha!=255 && alpha==Material::AlphaFunc::Solid) {
    alpha = Material::AlphaFunc::Transparent;
    }

  if((alpha==Material::AlphaFunc::AlphaTest && !requireAlphaTest) || alpha==Material::AlphaFunc::Transparent) {
    if(tex!=nullptr && tex->format()==Tempest::TextureFormat::DXT1 && clrAlpha==255) {
      alpha = Material::AlphaFunc::Solid;
      }
    }

  if(alpha==Material::AlphaFunc::AlphaTest || alpha==Material::AlphaFunc::AdditiveLight) {
    // castle wall in G1 (OW_DIRTDECAL.TGA) has alpha==0, set it to mul for now
    // if(clrAlpha==0) {
    //   alpha = Material::AlphaFunc::Multiply;
    //   }
    }

  if(alpha == Material::AlphaFunc::AlphaTest && !enableAlphaTest) {
    alpha = Material::AlphaFunc::Solid;
    }
  return alpha;
  }

void Material::loadFrames(const asset::Material& m) {
  loadFrames(m.texture, m.texture_anim_fps);
  }

void Material::loadFrames(const std::string_view fr, float fps) {
  frames = Resources::loadTextureAnim(fr);
  if(frames.empty())
    return;
  if(fps > 0)
    texAniFPSInv = uint64_t(1000.0f / fps); else
    texAniFPSInv = 1000/std::max<size_t>(frames.size(),1);
  }
