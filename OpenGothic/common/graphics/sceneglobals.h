#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include "graphics/dynamic/frustrum.h"
#include "rtscene.h"
#include "resources.h"

class Sky;
class WorldView;

class SceneGlobals final {
  public:
    SceneGlobals();
    ~SceneGlobals();

    enum VisCamera : uint8_t {
      V_Shadow0    = 0,
      V_Shadow1    = 1,
      V_ShadowLast = 1,
      V_Main       = 2,
      V_HiZ        = 3,
      V_Vsm        = 4,
      V_Count
      };

    // UboGlobal::viewFlags, as VF_* in scene.glsl
    enum ViewFlag : uint32_t {
      VF_HIZ = 0x1,
      VF_TAA = 0x2,
      };

    static bool isShadowView(VisCamera v);

    void setViewProject(const Tempest::Matrix4x4& view, const Tempest::Matrix4x4& proj,
                        float zNear, float zFar,
                        const Tempest::Matrix4x4 *sh);
    void setViewLwc(const Tempest::Matrix4x4& view, const Tempest::Matrix4x4& proj, const Tempest::Matrix4x4 *sh);
    void setViewVsm(const Tempest::Matrix4x4& view, const Tempest::Matrix4x4& viewLwc);
    // TAA: the sub-pixel shift J of the main projection (J*proj), taken by the next setViewProject/setViewLwc;
    // nullptr leaves the projection as it is
    void setJitter(const Tempest::Matrix4x4* jitter);
    void setSky(const Sky& s);
    void setWorld(const WorldView& wview);
    void setUnderWater(bool w);
    // TAA: the bias of the material textures' mip level in the main view; shadows keep 0
    void setMipBias(float bias);
    void setTaaFrame(bool taa, bool lampTaa, uint32_t frame);
    // motion vectors of objects: on/off and the camera's part (identity and this frame's camera on a cut)
    void setMotion(bool enabled, const Tempest::Matrix4x4& reprojectClip, const Tempest::Matrix4x4& viewProjectPrev);
    // the TAA reactive mask (graphics plan 1v): the added light on screen that makes an additive pixel fully reactive
    void setTaaReactiveLref(float lref);
    // VSM lamps: an object nearer than this to a light (cm, along its cube face's axis) is no blocker of it
    void setVsmLampNear(float cm);
    // VSM lamps: the sides of a surface a cube face keeps: 1 the far one, 2 both for the world mesh, 3 for objects too
    void setVsmLampSides(uint32_t sides);
    // bench `vsm study <bits>`: the shadow test on a mesh's facets, VSM_STUDY_* of scene.glsl (0: the game's)
    void setVsmStudy(uint32_t bits);

    void setTime(uint64_t time);
    void commitUbo(uint8_t fId);
    void prepareGlobals(Tempest::Encoder<Tempest::CommandBuffer> &cmd, uint8_t frameId);

    void setResolution(uint32_t w, uint32_t h);
    void setHiZ(const Tempest::Texture2d& hiZ);
    void setShadowMap(const Tempest::Texture2d* tex[]);
    void setFog(const Tempest::StorageImage* lut, const Tempest::StorageImage* lutMs);

    void setVirtualShadowMap(bool enabled,
                             const Tempest::ZBuffer&       vsmPageData,
                             const Tempest::StorageImage&  pageTbl,
                             const Tempest::StorageImage&  pageHiZ,
                             const Tempest::StorageBuffer& vsmPageList);

    const Tempest::Matrix4x4& viewProject() const;
    const Tempest::Matrix4x4& viewProjectInv() const;
    const Tempest::Matrix4x4& viewShadow(uint8_t view) const;
    const Tempest::Vec3       clipInfo() const;

    const Tempest::Matrix4x4  viewProjectLwc() const;
    const Tempest::Matrix4x4  viewProjectLwcInv() const;

    uint64_t                          tickCount = 0;
    const Tempest::Texture2d*         shadowMap[2] = {};

    Tempest::Matrix4x4                view, proj;
    Tempest::Matrix4x4                viewLwc;
    Tempest::Vec3                     originLwc;
    float                             znear = 0;

    const Tempest::Texture2d*         sceneColor   = &Resources::fallbackBlack();
    const Tempest::Texture2d*         sceneDepth   = &Resources::fallbackBlack();
    const Tempest::Texture2d*         zbuffer      = &Resources::fallbackBlack();

    const Tempest::Texture2d*         gbufDiffuse  = &Resources::fallbackBlack();
    const Tempest::Texture2d*         gbufNormals  = &Resources::fallbackBlack();

    const Tempest::Texture2d*         hiZ          = &Resources::fallbackTexture();

    const Tempest::StorageImage*      fogLut       = &Resources::fallbackImage3d();
    const Tempest::StorageImage*      fogLutMs     = &Resources::fallbackImage3d();

    const Tempest::StorageBuffer*     lights        = nullptr;

    const Tempest::Texture2d*         vsmPageData   = nullptr;
    const Tempest::StorageImage*      vsmPageTbl    = nullptr;
    const Tempest::StorageImage*      vsmPageHiZ    = nullptr;
    const Tempest::StorageBuffer*     vsmPageList   = nullptr;
    bool                              vsmEnabled    = false;
    Tempest::StorageImage             vsmDbg;

    struct UboGlobal final {
      Tempest::Matrix4x4              viewProject;
      Tempest::Matrix4x4              viewProjectInv;
      Tempest::Matrix4x4              viewShadow[Resources::ShadowLayers];
      Tempest::Matrix4x4              viewProjectLwcInv;
      Tempest::Matrix4x4              viewShadowLwc[Resources::ShadowLayers];
      Tempest::Matrix4x4              viewVirtualShadow;
      Tempest::Matrix4x4              viewVirtualShadowLwc;
      Tempest::Matrix4x4              viewProject2VirtualShadow;
      Tempest::Vec4                   vsmDdx, vsmDdy;
      Tempest::Matrix4x4              view, project, projectInv;
      Tempest::Vec3                   sunDir        = {0,0,1};
      float                           waveAnim      = 0;
      Tempest::Vec3                   lightAmb      = {1,1,1};
      float                           exposure      = 1;
      Tempest::Vec3                   lightCl       = {0,0,0};
      float                           GSunIntensity = 0;
      Tempest::Vec4                   frustrum[6];
      Tempest::Vec3                   clipInfo;
      uint32_t                        tickCount32 = 0;
      Tempest::Vec3                   camPos;
      float                           isNight = 0;
      Tempest::Vec2                   screenResInv;
      Tempest::Vec2                   closeupShadowSlice;

      Tempest::Vec3                   pfxLeft  = {};
      uint32_t                        underWater = 0;
      Tempest::Vec3                   pfxTop   = {};
      float                           luminanceMed = 0;
      Tempest::Vec3                   pfxDepth = {};
      float                           plPosY = {};
      Tempest::Point                  hiZTileSize = {};
      Tempest::Point                  screenRes = {};
      Tempest::Vec2                   cloudsDir[2] = {};

      float                           probeLodScale = 0.002f; // 1/cm: probeGridLod in probe_common.glsl
      uint32_t                        fogQuality = 0;
      float                           mipBias      = 0; // TAA: the main view only (commitUbo), 0 in shadows
      float                           mipGradScale = 1; // exp2(mipBias)
      uint32_t                        taaFrame     = 0; // TAA: the frame number (Renderer::prepareTaa), 0 without TAA
      uint32_t                        taaActive    = 0; // the lamp taps turn by taaFrame (light.frag)
      // motion vectors of objects (graphics plan 1b): the main view only (commitUbo), 0 in the other views.
      // Both matrices carry this frame's jitter, so the residual has none (Taa::motionResidual)
      uint32_t                        motionVectors = 0;
      float                           reactiveLrefInv = 4.f; // TAA reactive mask: 1/Lref of added light (main.frag)
      Tempest::Matrix4x4              reprojectClip   = Tempest::Matrix4x4::mkIdentity(); // Taa::reprojection
      Tempest::Matrix4x4              viewProjectPrev = Tempest::Matrix4x4::mkIdentity(); // Taa::viewProjectPrev
      float                           vsmLampNear     = 30.f; // cm: VSM lamps, the fixture plane (main.vert)
      uint32_t                        vsmLampSides    = 3;    // VSM lamps: the sides a cube face keeps (main.vert)
      uint32_t                        vsmStudy        = 0;    // bench `vsm study`: VSM_STUDY_* of scene.glsl
      uint32_t                        viewFlags       = 0; // VF_* of scene.glsl: VF_HIZ in V_HiZ only (commitUbo), VF_TAA
      };

    Tempest::UniformBuffer<UboGlobal> uboGlobalPf[Resources::MaxFramesInFlight][V_Count];
    Tempest::StorageBuffer            uboGlobal[V_Count];

    Frustrum                          frustrum[V_Count];

    bool                              zWindEnabled = false;
    bool                              legacyWind = true;
    float                             windScale = 1.f;
    uint64_t                          windPeriod = 6000;

    RtScene                           rtScene;

  private:
    UboGlobal                         uboGlobalCpu;
    Tempest::Matrix4x4                jitter;
    bool                              jitterEnabled = false;

    Tempest::Matrix4x4                jittered(const Tempest::Matrix4x4& proj) const;
  };

