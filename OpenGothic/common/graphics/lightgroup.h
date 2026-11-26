#pragma once

#include <Tempest/CommandBuffer>
#include <unordered_set>
#include "asset/worldtypes.h"

#include "lightsource.h"
#include "resources.h"

class DbgPainter;
class SceneGlobals;
class World;

class LightGroup final {
  public:
    LightGroup(const SceneGlobals& scene);

    class Light final {
      public:
        Light() = default;
        Light(Light&& other);
        Light& operator = (Light&& other);
        ~Light();

        void     setPosition(float x, float y, float z);
        void     setPosition(const Tempest::Vec3& p);
        bool     renderPosition(Tempest::Vec3& out) const;

        void     setEnabled(bool e);
        bool     isEnabled() const;

        void     setRange (float r);
        void     setColor (const Tempest::Vec3& c);
        void     setColor (const std::vector<Tempest::Vec3>& c, float fps, bool smooth);
        void     setTimeOffset(uint64_t t);
        void     setKeysOnce(bool once);
        // the yaw of the light's own axes (radians round world Y): its light mask turns with it
        void     setYaw(float yaw);

        uint64_t effectPrefferedTime() const;

      private:
        Light(LightGroup& l, size_t id):owner(&l), id(id) {}
        LightGroup* owner = nullptr;
        size_t      id    = 0;

      friend class LightGroup;
      };

    Light  add(const asset::LightPreset& vob);
    Light  add(const asset::VLight& vob);
    Light  add(std::string_view preset);
    size_t size() const { return lightSourceData.size(); }
    // the count as of the last prepareGlobals: what recording draws, while tick may add or free lights
    size_t publishedCount() const { return pubCount; }

    void   tick(uint64_t time);
    bool   updateLights();
    auto&  lightsSsbo() const { return lightSourceSsbo; }

    void   prepareGlobals(Tempest::Encoder<Tempest::CommandBuffer> &cmd, uint8_t fId);

    void   dbgLights(DbgPainter& p) const;

    // Light masks (cookies) of own lights, by slot 1..MaxCookies; an empty slot holds white (light.frag binds them all)
    static constexpr size_t MaxCookies = 4;
    // the cookie handles as of the last prepareGlobals, copied by value (ref-counted): recording binds these while
    // tick/streaming may load a new mask into cookieTex or free a light. The white fallback is loaded at prep, not record
    const Tempest::Texture2d& publishedCookie(size_t slot) const;

    struct Near {
      Tempest::Vec3 pos;
      float         range = 0;
      Tempest::Vec3 color;
      };
    // lights whose sphere comes within reach of at (smoke and steam take their light, pfxbucket.cpp)
    void   nearLights(const Tempest::Vec3& at, float reach, std::vector<Near>& out) const;

  private:
    using Vertex = Resources::VertexL;

    struct Path {
      uint32_t dst;
      uint32_t src;
      uint32_t size;
      };

    struct LightSsbo {
      Tempest::Vec3 pos;
      float         range  = 0;
      Tempest::Vec3 color;
      float         cookie = 0; // scene.glsl LightSource
      float         sourceSize = 6;  // cm, VobLayer::DefaultLightSourceCm
      float         nearLimit  = 0;  // the lamp's own limit of the light near it (vobs.d "near"), 0: the common one
      float         padding[2] = {};
      };

    struct VsmSsbo {
      uint32_t mask[6];
      };

    size_t                     alloc(bool dynamic);
    void                       free(size_t id);

    void                       markAsDurty(size_t id);
    void                       markAsDurtyNoSync(size_t id);
    void                       resetDurty();

    const asset::LightPreset& findPreset(std::string_view preset) const;

    std::vector<asset::LightPreset> presets;
    uint32_t                   cookieSlot(std::string_view name);
    std::vector<std::string>         cookieNames;
    const Tempest::Texture2d*        cookieTex[MaxCookies] = {};

    mutable std::mutex               sync;
    std::vector<size_t>              freeList;
    std::vector<LightSource>         lightSourceDesc;
    std::vector<LightSsbo>           lightSourceData;
    size_t                           pubCount = 0;
    Tempest::Texture2d               pubCookie[MaxCookies];
    Tempest::Texture2d               pubCookieWhite;
    std::unordered_set<size_t>       animatedLights;
    std::vector<uint32_t>            duryBit;

    Tempest::StorageBuffer           lightSourceSsbo;
    Tempest::StorageBuffer           patchSsbo[Resources::MaxFramesInFlight];
  };

