#pragma once

#include <Tempest/RenderPipeline>
#include <Tempest/StorageBuffer>
#include <vector>

#include "sceneglobals.h"

class VisualObjects;
class DrawBuckets;
class DrawClusters;

class DrawCommands {
  public:
    // Meshlet-page pairs the VSM visibility pass may emit a frame: VSM_MAX_MESHLETS in vsm_common.glsl
    static constexpr uint32_t VsmMaxMeshlets = 512*1024;
    // the sun's virtual shadow map for a forward-lit draw, or the 1x1x1 table that sends it to the cascades
    static void bindSunVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene,
                           uint32_t tblSlot, uint32_t atlasSlot);

    enum Type : uint8_t {
      Landscape,
      Static,
      Movable,
      Animated,
      Pfx,
      Morph,
      };
    static uint32_t hiddenTypes; // debug (test bench): a bit per Type that is not drawn
    static bool     hidePfxObjects; // debug (test bench), `view hide pfx` only: the particles of PfxObjects too

    struct DrawCmd {
      const Tempest::RenderPipeline* pMain        = nullptr;
      const Tempest::RenderPipeline* pShadow      = nullptr;
      const Tempest::RenderPipeline* pVsm         = nullptr;
      const Tempest::RenderPipeline* pHiZ         = nullptr;
      Type                           type         = Type::Landscape;

      Material::AlphaFunc            alpha        = Material::Solid;
      uint32_t                       firstPayload = 0;
      uint32_t                       maxPayload   = 0;

      // bindfull only
      uint32_t                       bucketId     = 0;

      bool                           isForwardShading() const;
      bool                           isShadowmapRequired() const;
      bool                           isSceneInfoRequired() const;
      bool                           isTextureInShadowPass() const;
      bool                           isBindless() const;
      bool                           isMeshShader() const;
      };

    DrawCommands(VisualObjects& owner, DrawBuckets& buckets, DrawClusters& clusters, const SceneGlobals& scene);
    ~DrawCommands();

    const DrawCmd& operator[](size_t i) const { return cmd[i]; }

    void     commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    uint16_t commandId(const Material& m, Type type, uint32_t bucketId);
    void     addClusters(uint16_t cmdId, uint32_t meshletCount);

    void     visibilityPass(Tempest::Encoder<Tempest::CommandBuffer>& cmd, int pass);
    void     visibilityVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd);

    void     drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd);
    void     drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, SceneGlobals::VisCamera viewId, Material::AlphaFunc func);

    void     drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd);

  private:
    enum TaskLinkpackage : uint8_t {
      T_Scene      = 0,
      T_Payload    = 1,
      T_Instance   = 2,
      T_Bucket     = 3,
      T_Indirect   = 4,
      T_Clusters   = 5,
      T_Lights     = 6,
      T_HiZ        = 7,
      T_VsmPages   = 8,
      T_CmdOffsets = 9,
      T_Order      = T_Lights, // visibility_pass*_ordered.comp: the clusters in their canonical order
      };

    enum UboLinkpackage : uint8_t {
      L_Scene      = 0,
      L_Payload    = 1,
      L_Instance   = 2,
      L_Pfx        = L_Instance,
      L_Bucket     = 3,
      L_Ibo        = 4,
      L_Vbo        = 5,
      L_Diffuse    = 6,
      L_Sampler    = 7,
      L_Shadow0    = 8,
      L_Shadow1    = 9,
      L_MorphId    = 10,
      L_Morph      = 11,
      L_SceneClr   = 12,
      L_GDepth     = 13,
      L_CmdOffsets = 14,
      L_Normal     = 15,
      L_Orm        = 16,
      L_FogLut     = 17,
      L_FogLutMs   = 18,
      L_VsmPageTbl = 19,
      L_VsmPageAtlas = 20,
      L_InstancePrev = 21, // the instance buffer after the previous frame (motion vectors, G-buffer pass)
      L_FogOcc     = 22, // the sun's bits along the ray of the fog (fog3d.comp), for the fog of translucent under GI
      L_VsmPages   = L_Shadow0,
      L_Lights     = L_Shadow1,
      };

    struct IndirectCmd {
      uint32_t vertexCount   = 0;
      uint32_t instanceCount = 0;
      uint32_t firstVertex   = 0;
      uint32_t firstInstance = 0;
      uint32_t writeOffset   = 0;
      };

    struct View {
      SceneGlobals::VisCamera viewport = SceneGlobals::V_Main;
      Tempest::StorageBuffer  visClusters, indirectCmd;
      Tempest::StorageBuffer  vsmClusters;
      };

    bool                     isViewEnabled(SceneGlobals::VisCamera v) const;
    const Tempest::StorageBuffer& orderedClusters();

    void                     setBindings(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const DrawCmd& cx, SceneGlobals::VisCamera viewId);

    VisualObjects&           owner;
    DrawBuckets&             buckets;
    DrawClusters&            clusters;
    const SceneGlobals&      scene;
    size_t                   maxPayload = 0;

    std::vector<DrawCmd>     cmd;
    std::vector<DrawCmd*>    ord;
    bool                     cmdDurtyBit = false;
    View                     views[SceneGlobals::V_Count];

    const bool               vsmSupported;

    // bench `deterministic on`: cluster ids sorted by what they draw, not by the order they were allocated in
    std::vector<uint32_t>    orderIds, orderNext;
    Tempest::StorageBuffer   orderSsbo;
    Tempest::StorageBuffer   vsmIndirectCmd;
  };
