#pragma once

#include <Tempest/RenderState>

#include "records/definitions.h"

#include <Tempest/Texture2d>

#include "graphics/visualfx.h"
#include "graphics/material.h"
#include "graphics/pfx/pfxmotion.h"

class PfxEmitterMesh;

class ParticleFx final {
  public:
    ParticleFx(const Material& mat, const asset::VirtualObject& vob);
    ParticleFx(const Def::ParticleEffect& src, std::string_view name);
    ParticleFx(const ParticleFx& proto, const VisualFx::Key& key);

    enum class EmitterType:uint8_t {
      Point,
      Line,
      Box,
      Circle,
      Sphere,
      Mesh
      };

    enum class Dir:uint8_t {
      Rand,
      Dir,
      Target
      };

    enum class Frame:uint8_t {
      World,
      Object,
      Node,
      };

    enum class Distribution:uint8_t {
      Rand,
      Dir,
      Uniform,
      Walk
      };

    enum class Orientation:uint8_t {
      None,
      Velocity,
      Velocity3d,
      };

    using KeyList = std::vector<float>;

    std::string   dbgName;

    float         ppsValue            = 0;
    KeyList       ppsScaleKeys;
    bool          ppsIsLooping        = false;
    bool          ppsIsSmooth         = false;
    float         ppsFPS              = 0;
    const ParticleFx* ppsCreateEm     = nullptr;
    uint64_t      ppsCreateEmDelay    = 0;

    EmitterType   shpType             = EmitterType::Point;
    Frame         shpFOR              = Frame::Object;
    Tempest::Vec3 shpOffsetVec;
    Distribution  shpDistribType      = Distribution::Rand;
    float         shpDistribWalkSpeed = 0.f;
    bool          shpIsVolume         = false;
    Tempest::Vec3 shpDim;
    const PfxEmitterMesh* shpMesh     = nullptr;
    std::string   shpMesh_S;
    bool          shpMeshRender       = false;
    KeyList       shpScaleKeys;
    bool          shpScaleIsLooping   = false;
    bool          shpScaleIsSmooth    = false;
    float         shpScaleFPS         = 0.f;

    Dir           dirMode             = Dir::Rand;
    Frame         dirFOR              = Frame::Object;
    Frame         dirModeTargetFOR    = Frame::Object;
    Tempest::Vec3 dirModeTargetPos;
    float         dirAngleHead        = 0.f;
    float         dirAngleHeadVar     = 0.f;
    float         dirAngleElev        = 0.f;
    float         dirAngleElevVar     = 0.f;

    float         velAvg              = 0.f;
    float         velVar              = 0.f;
    float         lspPartAvg          = 0.f;
    float         lspPartVar          = 0.f;

    Tempest::Vec3 flyGravity;
    bool          flyCollDet = false;

    Material      visMaterial;
    Orientation   visOrientation        = Orientation::None;
    bool          visTexIsQuadPoly      = true;
    float         visTexAniFPS          = 0.f;
    bool          visTexAniIsLooping    = false;
    Tempest::Vec3 visTexColorStart;
    Tempest::Vec3 visTexColorEnd;
    Tempest::Vec2 visSizeStart;
    float         visSizeEndScale      = 0.f;
    float         visAlphaStart        = 0.f;
    float         visAlphaEnd          = 0.f;
    bool          visYawAlign          = false;
    bool          visZBias             = false;
    // own atlas flipbook (particles.d "flipbook"): cols/rows 1..8, 0 frames when off
    uint8_t       flipCols   = 0;
    uint8_t       flipRows   = 0;
    uint8_t       flipFrames = 0;
    float         flipFps    = 0.f;
    bool          flipSteady = false;  // ParticleContent::Flipbook::steady
    bool          flipOnce   = false;  // ParticleContent::Flipbook::once
    std::vector<const ParticleFx*> with; // particles.d "with": effects an emitter of this one brings along
    // particles.d "land": the effect put (with its "with") where a particle meets the world's static geometry; the
    // particle stops there, its sprite goes and its trail draws in (PfxObjects::ground)
    const ParticleFx* land = nullptr;
    // own motion and look (particles.d "curl", "drag", "stretch", "minPixels"; ParticleContent::Motion)
    PfxMotion::Params motion;
    float         stretchMs = 0.f;  // a velocity-oriented sprite grows by |v|*stretchMs along its path
    float         minPixels = 0.f;  // a smaller sprite is drawn this size on screen, its alpha down by the area
    // particles.d "lean" of a steady card and "inherit" of a living particle (ParticleContent::Motion)
    bool          own            = false; // declared in particles.d
    float         leanRise       = 0.f;  // cm/ms; 0: off
    float         leanMaxTan     = 0.f;
    float         leanResponseMs = 0.f;
    float         leanAlong      = 0.75f;
    float         leanGrowMs     = 0.f;
    float         leanGrowFrom   = 1.f;
    float         inherit        = 0.f;
    float         plumeCm        = 0.f;  // particles.d "plume": cm over the fire; 0: off
    float         plumeRise      = 0.f;  // cm/ms
    float         plumeWind      = 0.f;  // the flame's share of the wind over this effect's own
    float         wind           = 0.f;  // particles.d "wind": the share of the world's wind
    // particles.d "hdr" (ParticleContent::Motion): the night's gain of the emission over a texel's brightness
    float         hdrFrom   = 0.f;
    float         hdrTo     = 0.f;  // 0: off
    float         hdrEdge   = 1.f;
    // particles.d "haze": the shimmer of hot air, cm; hazeShift 0: off. Such an effect is Material::Ghost
    float         hazeShift = 0.f;
    float         hazeCell  = 0.f;
    // particles.d "light" (ParticleContent::Motion): the light a spell effect of this one brings; lightRange 0: off
    float         lightRange = 0.f;
    Tempest::Vec3 lightColor;
    KeyList       lightKeys;
    float         lightFps   = 0.f;

    const Tempest::Texture2d* trlTexture = nullptr;
    float         trlFadeSpeed         = 0.f;
    float         trlWidth             = 0.f;

    const Tempest::Texture2d* mrkTexture = nullptr;
    float         mrkFadeSpeed         = 0.f;
    float         mrkSize              = 0.f;

    std::string   flockMode;
    float         flockStrength = 0;

    bool          useEmittersFOR = true;

    std::string   timeStartEnd_S;
    bool          m_bIsAmbientPFX=false;

    uint64_t      prefferedTime = 0;

    bool          isDecal() const;
    bool          hasTrails() const;
    // the weight of the TAA reactive mask (graphics plan 1v): 1 for particles; 0 for rain and snow, thousands of
    // thin particles over the whole frame; a sprite or decal vob as a mesh of its material (Material::taaReactive)
    float         taaReactive() const;
    static bool   isPrecipitation(std::string_view name);

    uint64_t      maxLifetime() const;
    uint64_t      effectPrefferedTime() const;
    float         maxPps() const;
    float         shpScale(uint64_t time) const;
    float         ppsScale(uint64_t time) const;

  private:
    uint64_t             calcPrefferedTimeSingle() const;
    static uint64_t      calcPrefferedTimeSingle(const KeyList& k, float fps);

    static auto          loadTexture(std::string_view src) -> const Tempest::Texture2d*;
    static KeyList       loadArr(std::string_view src);
    static EmitterType   loadEmitType(std::string_view src);
    static Frame         loadFrameType(std::string_view src);
    static Distribution  loadDistribType(std::string_view src);
    static Dir           loadDirType(std::string_view src);
    static Orientation   loadOrientation(std::string_view src);

    float                fetchScaleKey(uint64_t time, const KeyList& k, float fps, bool smooth, bool loop) const;
  };

