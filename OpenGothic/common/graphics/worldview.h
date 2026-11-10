#pragma once

#include <Tempest/Device>
#include <Tempest/Shader>

#include "asset/worldtypes.h"

#include "graphics/mesh/landscape.h"
#include "graphics/meshobjects.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/pfx/pfxobjects.h"
#include "graphics/sky/sky.h"
#include "lightsource.h"
#include "lightgroup.h"
#include "sceneglobals.h"
#include "visualobjects.h"

class World;
class Camera;
class ParticleFx;
class PackedMesh;
class gtime;
class ChunkResidency;
class ObjVisual;
struct ChunkId;
namespace asset { struct WorldMesh; }

class WorldView {
  public:
    void probeMemory(std::string_view stage) const;

    WorldView(const PackedMesh& wmesh, std::string_view skyPreset);
    WorldView(std::pair<Tempest::Vec3,Tempest::Vec3> bounds, std::string_view skyPreset);
    ~WorldView();

    void startLandscapeStreaming(asset::WorldMesh&& mesh, ChunkResidency& residency, double preloadMs);
    void startVobStreaming(ChunkResidency& residency);
    void setTextureResidency(ChunkResidency* residency) { visuals.setTextureResidency(residency); }
    void prepareLandscapeWork();
    void finishLandscapeUpdate();
    bool landscapeReady() const;
    std::string landscapeWaitState() const;

    const LightSource&        mainLight() const;
    const Tempest::Vec3&      ambientLight() const;
    std::pair<Tempest::Vec3, Tempest::Vec3> bbox() const;

    bool isInPfxRange(const Tempest::Vec3& pos) const;

    void preFrameUpdate(const Camera& camera, uint64_t tickCount, uint8_t fId);
    void postFrameupdate();

    void prepareGlobals(Tempest::Encoder<Tempest::CommandBuffer> &cmd, uint8_t fId);

    void setGbuffer(const Tempest::Texture2d& diffuse,
                    const Tempest::Texture2d& norm);
    void setShadowMaps (const Tempest::Texture2d* shadow[]);
    void setVirtualShadowMap(bool enabled,
                             const Tempest::ZBuffer& pageData,
                             const Tempest::StorageImage& pageTbl,
                             const Tempest::StorageImage& pageHiZ,
                             const Tempest::StorageBuffer& pageList);
    void setHiZ(const Tempest::Texture2d& hiZ);
    void setSceneImages(const Tempest::Texture2d& clr, const Tempest::Texture2d& depthAux, const Tempest::ZBuffer& depthNative);
    // TAA: the sub-pixel shift of the main projection for the next preFrameUpdate (nullptr: none)
    void setJitter(const Tempest::Matrix4x4* jitter) { sGlobal.setJitter(jitter); }
    void setMipBias(float bias) { sGlobal.setMipBias(bias); }
    void setTaaFrame(bool active, uint32_t frame) { sGlobal.setTaaFrame(active, frame); }
    // motion vectors of objects (graphics plan 1b): the previous copy of the instance buffer and the scene's part;
    // before preFrameUpdate, so that this frame's commit already keeps the history
    void setMotionVectors(bool enabled, const Tempest::Matrix4x4& reprojectClip, const Tempest::Matrix4x4& viewProjectPrev);
    void setTaaReactiveLref(float lref);
    void setVsmLampNear(float cm) { sGlobal.setVsmLampNear(cm); }
    void setWindEnabled(bool enabled, uint64_t period);
    void setFog(const Tempest::StorageImage& lut, const Tempest::StorageImage& lutMs);
    void setClothWind(bool enabled, float scale);
    void setOriginalClothMesh(const PackedMesh& mesh);
    void setOriginalClothVisible(bool visible);

    void dbgLights      (DbgPainter& p) const;

    bool updateLights(const gtime gameTime);
    void updateRtScene(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

    void updateFrustrum (const Frustrum fr[]);
    void visibilityPass (Tempest::Encoder<Tempest::CommandBuffer>& cmd, int pass);
    void visibilityVsm  (Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    void drawHiZ        (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawShadow     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t frameId, uint8_t layer);
    void drawVsm        (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawGBuffer    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t frameId);
    void drawWater      (Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void drawMultiply   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t frameId);
    void drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t frameId);

    MeshObjects::Mesh   addView      (std::string_view visual, int32_t headTex, int32_t teethTex, int32_t bodyColor);
    MeshObjects::Mesh   addView      (const ProtoMesh* visual, int32_t material = 0);
    MeshObjects::Mesh   addItmView   (std::string_view visual, int32_t material);
    MeshObjects::Mesh   addAtachView (const ProtoMesh::Attach& visual, const int32_t version);
    MeshObjects::Mesh   addStaticView(const ProtoMesh* visual, bool staticDraw = false);
    MeshObjects::Mesh   addStaticView(std::string_view visual);
    MeshObjects::Mesh   addDecalView (const asset::VisualDecal& vob);
    LightGroup::Light   addLight     (const asset::VLight& vob, const uint64_t timeOffset);
    LightGroup::Light   addLight     (std::string_view preset, const uint64_t timeOffset);

    void                dbgClusters(Tempest::Painter& p, Tempest::Vec2 wsz);

    const SceneGlobals& sceneGlobals() const { return sGlobal; }
    const Sky&          sky() const { return gSky; }
    const LightGroup&   lights() const { return gLights; }
    const DrawClusters& clusters() const;
    const DrawBuckets&  drawBuckets() const;

  private:
    std::pair<Tempest::Vec3, Tempest::Vec3> aabb;
    SceneGlobals  sGlobal;
    Sky           gSky;
    LightGroup    gLights;
    VisualObjects visuals;

    MeshObjects   objGroup;
    PfxObjects    pfxGroup;
    std::unique_ptr<Landscape> land;
    std::unique_ptr<Landscape> originalCloth;
    struct LandscapeStreaming;
    std::unique_ptr<LandscapeStreaming> streaming;
    struct VobStreaming;
    std::shared_ptr<VobStreaming> vobStreaming;
    void addVob(ObjVisual& visual, ChunkId id);
    void removeVob(ObjVisual& visual, bool release = true);
    void moveVob(ObjVisual& visual, ChunkId id);
    void replaceVob(ObjVisual& previous, ObjVisual& next);

    bool needToUpdateCmd(uint8_t frameId) const;
    void invalidateCmd();

  friend class LightGroup::Light;
  friend class PfxEmitter;
  friend class TrlObjects;
  friend class ObjVisual;
  friend struct MeshGltfTest;
  };
