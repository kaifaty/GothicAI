#pragma once

#include <Tempest/Texture2d>

#include <array>

#include "asset/material.h"
#include "asset/worldtypes.h"
#include "records/definitions.h"
#include "graphics/foliagetree.h"
#include "graphics/grass/grassground.h"
#include "graphics/grass/grassparams.h"
#include "graphics/grass/grasstrack.h"
#include "graphics/windowglow.h"

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

    // "two_sided" of a record: an object's mesh is drawn, casts its shadow and is seen by rays from both sides
    // (reports/engine/2026-11-16-modern-trees/two-sided.md). Same: the side away from the winding is lit by the
    // vertex normal as it is, as the second copy of a card listed twice was; Flip: by the normal turned to the eye
    enum class TwoSided : uint8_t {
      Off,
      Same,
      Flip,
      };

    enum class Antitile : uint8_t {
      Off,
      Offset,
      Rotate,
      };

    // materials.json "fringe": a second layer (grass) over a projected material, where the mesh's v says it is
    // (material_projection.glsl). Its maps by world position at their own tile; height in the ORM's blue
    struct Fringe final {
      const Tempest::Texture2d* albedo = nullptr;
      const Tempest::Texture2d* normal = nullptr;
      const Tempest::Texture2d* orm    = nullptr;
      float tile      = 0;        // metres per repeat; 0: no layer
      float top[2]    = {0, 0};   // shares of v from the top edge (v=0): solid to [0], tufts end at [1]
      float bottom[2] = {0, 0};   // the same from the foot (v=1)
      bool  operator==(const Fringe&) const = default;
      bool  present() const { return tile>0 && albedo!=nullptr && normal!=nullptr && orm!=nullptr; }
      };

    // materials.json "ground" of a texture with `grass`: the meadow's two layers by world position (grass/
    // grassground.h, shader/grass/grass_layers.glsl); "verge" of the ground beside it: the same maps and numbers of
    // the grass it is the verge of, and that grass's packed record
    // "ground.border": the earth of the meadow's notches by a road; "track" of a road (grass/grasstrack.h,
    // shader/grass/grass_track.glsl): its crust and loose earth by world position, the ribbon's three maps (tint,
    // normal, mask) in the mesh's uv
    struct Layers final {
      using Maps = std::array<const Tempest::Texture2d*,3>;
      const Tempest::Texture2d* sward[3] = {}; // albedo, normal, ORM with the height in its blue
      const Tempest::Texture2d* earth[3] = {};
      Maps                      border = {};
      Maps                      crust  = {}, loose = {}, ribbon = {};
      Grass::Ground             ground;
      Grass::Verge              verge;
      Grass::Track              track;
      float                     borderTile = 0.f; // metres per repeat; 0: the meadow's own earth at the border
      uint32_t                  grassBits = 0;
      bool  operator==(const Layers&) const = default;
      bool  present() const {
        return ground.on && sward[0]!=nullptr && sward[1]!=nullptr && sward[2]!=nullptr &&
               earth[0]!=nullptr && earth[1]!=nullptr && earth[2]!=nullptr;
        }
      bool  hasBorder() const { return borderTile>0.f && all(border); }
      bool  hasTrack() const { return track.on && all(crust) && all(loose) && all(ribbon); }
      static bool all(const Maps& m) { return m[0]!=nullptr && m[1]!=nullptr && m[2]!=nullptr; }
      };

    const Tempest::Texture2d* tex=nullptr;
    const Tempest::Texture2d* normal=nullptr;
    const Tempest::Texture2d* orm=nullptr;
    float                     uvScale          = 1.f;
    float                     antitileCell     = 1.f;
    Antitile                  antitile         = Antitile::Off;
    float                     projectionTile   = 0;  // metres per repeat by world position (materials.json); 0: mesh UV
    Fringe                    fringe;                // only with projection
    bool                     materialMaps     = false;
    std::vector<const Tempest::Texture2d*> frames;
    AlphaFunc                 alpha            = AlphaTest;
    float                     alphaWeight      = 1;
    Tempest::Point            texAniMapDirPeriod;
    uint64_t                  texAniFPSInv     = 1;
    bool                      isGhost          = false;
    float                     waveMaxAmplitude = 0;
    float                     turbidity        = 0;
    float                     turbidityDepth   = 0;
    float                     emissive         = 0;  // own glow, 0..1 of EmissiveMeshPeak (materials.json)
    bool                      hair             = false; // hair cards (materials.json): BK_HAIR
    bool                      strand           = false; // strands as ribbons (materials.json): BK_STRAND
    bool                      eye              = false; // an eyeball (materials.json): BK_EYE
    FoliageWind::Params       foliage;                 // leaves in the wind (materials.json): BK_FOLIAGE
    TwoSided                  twoSided         = TwoSided::Off; // both sides (materials.json): BK_TWO_SIDED, BK_TWO_SIDED_FLIP
    Grass::Params             grass;                   // a ground that grows grass (materials.json): BK_GRASS
    Layers                    layers;                  // its own two layers, or a verge's (materials.json): BK_GROUND, BK_VERGE
    WindowGlow::Params        window;                  // the glow is a window's (materials.json): BK_WINDOW
    float                     envMapping       = 0;

    bool operator == (const Material& other) const;

    bool isSolid() const;
    void setAlbedo(const Tempest::Texture2d* texture);

    bool hasMaterialMaps()       const { return materialMaps; }
    bool hasProjection()         const { return materialMaps && projectionTile>0; }
    // repeats per cm the shader projects by world position (BK_PROJECTION, opaque only); 0: by the mesh UV
    float projectionUvScale()    const;
    // the layer drawn over the projection (BK_FRINGE): it needs the projection and all three of its maps
    bool hasFringe()             const { return projectionUvScale()>0 && fringe.present(); }
    // UV units per metre the shader samples at, from the mesh's (TexelDensity): uv*uvScale, or by world position;
    // a morph mesh is drawn by its UV
    float textureUvPerMetre(float meshUvPerMetre, bool morph = false) const;
    bool hasFrameAnimation()     const { return !frames.empty() && texAniFPSInv!=0;    }
    bool hasUvAnimation()        const { return texAniMapDirPeriod!=Tempest::Point(0); }
    // the weight of the TAA reactive mask of a translucent mesh (graphics plan 1v): 1 when its texture changes by
    // itself (frames or scrolling UV: waterfalls, a barrier, a mesh flame), 0 for still glass and cobwebs
    float taaReactive()          const { return hasFrameAnimation() || hasUvAnimation() ? 1.f : 0.f; }
    bool isTesselated()          const { return isTesselated(alpha);                   }
    bool isForwardShading()      const { return isForwardShading(alpha);               }
    bool isSceneInfoRequired()   const { return isSceneInfoRequired(alpha);            }
    bool isShadowmapRequired()   const { return isShadowmapRequired(alpha);            }
    bool isTextureInShadowPass() const { return isTextureInShadowPass(alpha);          }
    bool isAlphaTestRequired()   const { return alphaTestRequired;                     } // explicit GLB MASK
    bool isFogRequired()         const { return isFogRequired(alpha);                  }

    static bool isTesselated(AlphaFunc alpha);
    static bool isForwardShading(AlphaFunc alpha);
    static bool isSceneInfoRequired(AlphaFunc alpha);
    static bool isShadowmapRequired(AlphaFunc alpha);
    static bool isTextureInShadowPass(AlphaFunc alpha);
    static bool isFogRequired(AlphaFunc alpha);

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

