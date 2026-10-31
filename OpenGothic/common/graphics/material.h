#pragma once

#include <Tempest/Texture2d>

#include "asset/material.h"
#include "asset/worldtypes.h"
#include <zenkit/DaedalusScript.hh>
#include <zenkit/addon/daedalus.hh>
#include "game/definitions/definitiontypes.h"

class Material final {
  public:
    Material()=default;
    Material(const asset::Material& m, bool enableAlphaTest, bool requireAlphaTest = false);
    Material(const asset::VisualDecal& decal);
    Material(const Def::ParticleEffect &src);

    enum AlphaFunc:uint8_t {
      Solid,
      AlphaTest,
      Water,
      Ghost,
      Multiply,
      Multiply2,
      Transparent,
      AdditiveLight,
      };

    enum class Antitile : uint8_t {
      Off,
      Offset,
      Rotate,
      };

    const Tempest::Texture2d* tex=nullptr;
    const Tempest::Texture2d* normal=nullptr;
    const Tempest::Texture2d* orm=nullptr;
    float                     uvScale          = 1.f;
    float                     antitileCell     = 1.f;
    Antitile                  antitile         = Antitile::Off;
    bool                      materialMaps     = false;
    std::vector<const Tempest::Texture2d*> frames;
    AlphaFunc                 alpha            = AlphaTest;
    float                     alphaWeight      = 1;
    Tempest::Point            texAniMapDirPeriod;
    uint64_t                  texAniFPSInv     = 1;
    bool                      isGhost          = false;
    float                     waveMaxAmplitude = 0;
    float                     envMapping       = 0;

    bool operator == (const Material& other) const;

    bool isSolid() const;
    void setAlbedo(const Tempest::Texture2d* texture);

    bool hasMaterialMaps()       const { return materialMaps; }
    bool hasFrameAnimation()     const { return !frames.empty() && texAniFPSInv!=0;    }
    bool hasUvAnimation()        const { return texAniMapDirPeriod!=Tempest::Point(0); }
    bool isTesselated()          const { return isTesselated(alpha);                   }
    bool isForwardShading()      const { return isForwardShading(alpha);               }
    bool isSceneInfoRequired()   const { return isSceneInfoRequired(alpha);            }
    bool isShadowmapRequired()   const { return isShadowmapRequired(alpha);            }
    bool isTextureInShadowPass() const { return isTextureInShadowPass(alpha);          }

    static bool isTesselated(AlphaFunc alpha);
    static bool isForwardShading(AlphaFunc alpha);
    static bool isSceneInfoRequired(AlphaFunc alpha);
    static bool isShadowmapRequired(AlphaFunc alpha);
    static bool isTextureInShadowPass(AlphaFunc alpha);

  private:
    friend struct AssetDigest;
    bool             alphaTestEnabled = false;
    // An explicit GLB MASK must survive DXT1's one-bit transparency and later
    // albedo changes; native materials retain their format-based heuristics.
    bool             alphaTestRequired = false;
    asset::AlphaFunction sourceAlphaFunc = asset::AlphaFunction::NONE;
    asset::MaterialGroup sourceMaterialGroup = asset::MaterialGroup::UNDEFINED;
    uint8_t          sourceColorAlpha = 255;
    // Keep explicit alpha overrides separate from texture-derived classification.
    AlphaFunc        classifiedAlpha = AlphaTest;

    static AlphaFunc loadAlphaFunc(asset::AlphaFunction zenAlpha, asset::MaterialGroup matGroup,
                                   uint8_t alpha, const Tempest::Texture2d* tex, bool enableAlphaTest,
                                   bool requireAlphaTest = false);
    void             loadFrames(const asset::Material& m);
    void             loadFrames(const std::string_view fr, float fps);
  };

