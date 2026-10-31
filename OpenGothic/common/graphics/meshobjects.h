#pragma once

#include <Tempest/Texture2d>
#include <Tempest/Matrix4x4>
#include <Tempest/UniformBuffer>

#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "visualobjects.h"

class Pose;
class ProtoMesh;
class LightSource;
class Npc;
namespace TestBench { bool ragdollBodyDrawable(const Npc& npc); }

class MeshObjects final {
  private:
    using Item = VisualObjects::Item;
    struct CpuNode {
      Bounds                  bounds;
      Material                material;
      Tempest::Matrix4x4      position;
      const StaticMesh*       mesh = nullptr;
      uint32_t                offset = 0, length = 0;
      };

  public:
    // A CPU owner stores attachment geometry without creating VisualObjects or GPU storage.
    MeshObjects()=default;
    MeshObjects(VisualObjects& parent);
    ~MeshObjects();

    class Mesh;
    class Node;

    class Node final {
      public:
        Node(Node&&)=default;

        const Bounds&      bounds() const;
        const Material&    material() const;
        Tempest::Matrix4x4 position() const;
        const StaticMesh*  mesh() const;

        std::pair<uint32_t,uint32_t> meshSlice() const;

      private:
        Node(const Item* it):it(it){}
        Node(const CpuNode* cpu):cpu(cpu){}
        const Item* it=nullptr;
        const CpuNode* cpu=nullptr;

      friend class Mesh;
      };

    class Mesh final {
      public:
        Mesh()=default;
        Mesh(MeshObjects& owner, const StaticMesh& mesh, int32_t version, bool staticDraw);
        Mesh(MeshObjects& owner, const StaticMesh& mesh, int32_t texVar, int32_t teethTex, int32_t bodyColor);
        Mesh(MeshObjects& owner, const ProtoMesh&  mesh, int32_t texVar, int32_t teethTex, int32_t bodyColor, bool staticDraw);
        Mesh(Mesh&& other);
        ~Mesh();
        Mesh& operator = (Mesh&& other);

        void   setObjMatrix(const Tempest::Matrix4x4& mt);
        void   setSkeleton (const Skeleton* sk);
        void   setPose     (const Tempest::Matrix4x4& obj, const Pose& p);
        void   setPose     (const Tempest::Matrix4x4& obj, const Tempest::Matrix4x4* bones);
        void   setAsGhost  (bool g);
        void   setFatness  (float f);
        void   setWind     (asset::AnimationType m, float intensity);
        void   setVisible  (bool visible);
        void   setDeformation(const std::vector<Tempest::Vec4>& vertices);
        // freeOnly: not in place of another anim of the head (a blink gives way to the faces)
        void   startMMAnim (std::string_view anim, float intensity, uint64_t timeUntil, bool freeOnly = false);
        void   setMMShape  (std::string_view anim, uint32_t frame0, uint32_t frame1, float alpha, float intensity);
        float  morphWeight (std::string_view anim) const;
        VisualObjects::MorphSample morphSample(std::string_view anim, uint64_t tick) const;

        bool   isEmpty()    const { return nodesCount()==0; }
        size_t nodesCount() const { return subCount+cpuSub.size(); }
        Node   node(size_t i) const { return cpuOnly ? Node(&cpuSub[i]) : Node(&sub[i]); }

        Bounds bounds() const;
        const ProtoMesh* protoMesh() const { return proto; }

        const PfxEmitterMesh* toMeshEmitter() const;

      private:
        void implSetObjMatrix(const Tempest::Matrix4x4& mt, const Tempest::Matrix4x4* tr);

        std::unique_ptr<Item[]> sub;
        size_t                  subCount=0;

        std::unique_ptr<InstanceStorage::Id> anim;

        const ProtoMesh*        proto   =nullptr;
        const Skeleton*         skeleton=nullptr;
        const AttachBinder*     binder  =nullptr;

        bool                    cpuOnly = false;
        std::vector<CpuNode>     cpuSub;
        friend bool TestBench::ragdollBodyDrawable(const Npc& npc);
      };

  private:
    VisualObjects*                  parent = nullptr;

    Item                            implGet(const StaticMesh& mesh, const StaticMesh::SubMesh& smesh,
                                            int32_t texVar, int32_t teethTex, int32_t bodyColor,
                                            bool staticDraw);

    const Tempest::Texture2d*       solveTex(const Tempest::Texture2d* def, std::string_view format,
                                             int32_t v,int32_t c);
  };
