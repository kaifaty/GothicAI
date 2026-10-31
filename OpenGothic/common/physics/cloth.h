#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CollisionWorld;

// Transient, world-attached Jolt soft bodies. No cloth state enters saves.
class Cloth final {
  public:
    using ShaderReference = std::function<Tempest::Vec3(uint32_t, const Tempest::Vec3&, const Tempest::Vec3&)>;
    using UpdateCallback = std::function<void(const std::vector<Tempest::Vec3>&, float)>;
    static constexpr float radiusCm = 4000.f;
    static constexpr float blendStartCm = 3500.f;
    static constexpr float maxVelocityMS = 15.f;

    class Instance final {
      public:
        ~Instance();
        Instance(const Instance&) = delete;
        void setMatrix(const Tempest::Matrix4x4& matrix);
        // A cheap local-space support guard. General world/character contacts
        // stay disabled; the visible cloth cannot cross its own narrow pole.
        void setAttachmentCapsule(const Tempest::Vec3& fromCm,const Tempest::Vec3& toCm,float radiusCm);
        void setLinearDamping(float perSecond);
        void setShaderReference(ShaderReference reference);
        void setUpdateCallback(UpdateCallback callback);
        const std::vector<Tempest::Vec3>& positionsCm() const;
        float simulationWeight() const;
        bool active() const;
      private:
        struct Impl;
        explicit Instance(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl;
        friend class Cloth;
      };

    struct WindOccluder {
      std::string body;
      Tempest::Vec3 pointCm;
      float distanceCm = 0;
      };
    struct Snapshot {
      uintptr_t id = 0;
      uint32_t zenId = uint32_t(-1);
      std::string visual;
      Tempest::Vec3 center;
      bool active = false, exposed = true, visible = true;
      size_t vertices = 0, pinned = 0;
      float simulationWeight = 0;
      float maxAnchorErrorCm = 0, maxSpeedCmPerSec = 0, maxSpeedEverCmPerSec = 0;
      float transitionMaxDeltaCm = 0;
      size_t contactVertices = 0;
      std::vector<std::string> contactBodies;
      uint32_t maxSpeedVertex = uint32_t(-1), maxSpeedEverVertex = uint32_t(-1);
      Tempest::Vec3 maxSpeedSourceCm, maxSpeedWorldCm, maxSpeedContactNormal;
      Tempest::Vec3 maxSpeedEverSourceCm, maxSpeedEverWorldCm, maxSpeedEverContactNormal;
      bool maxSpeedContact = false, maxSpeedEverContact = false;
      std::string maxSpeedContactBody, maxSpeedEverContactBody;
      std::vector<WindOccluder> windOccluders;
      uint64_t projectileHits = 0, arrowPasses = 0, recoveryCount = 0;
      uint64_t simulationSteps = 0;
      // Visible vertex order, already blended with the shader reference.
      std::vector<Tempest::Vec3> positionsCm, worldPositionsCm, referenceWorldPositionsCm, anchorsCm;
      std::vector<uint32_t> indices;
      };

    explicit Cloth(CollisionWorld& world);
    ~Cloth();
    Cloth(const Cloth&) = delete;
    std::shared_ptr<Instance> add(const std::vector<Tempest::Vec3>& localCm,
                                 const std::vector<uint32_t>& triangleIndices,
                                 const std::vector<uint32_t>& pinnedIndices,
                                 const Tempest::Matrix4x4& matrix, float stiffness = .8f,
                                 std::string visual = {}, uint32_t zenId = uint32_t(-1));
    void setEnabled(bool enabled);
    bool enabled() const;
    void setObserver(const Tempest::Vec3& worldCm);
    void setWind(const Tempest::Vec3& worldMetersPerSecond);
    void setWindScale(float scale);
    float windScale() const;
    // World portal-room classification. Attachments and ship geometry do not
    // occlude wind; a false result identifies an actual indoor BSP room.
    void setExposureTest(std::function<bool(const Tempest::Vec3&)> test);
    // The main camera supplies a conservative sphere test with a margin.
    // Without a renderer (contracts), all nearby cloth remains visible.
    void setVisibilityTest(std::function<bool(const Tempest::Vec3&,float)> test);
    std::vector<Snapshot> snapshots() const;
    void resetStats();
    size_t bodyCount() const;
    // Pass-through segment tests; cloth is deliberately absent from solid queries.
    size_t projectile(const Tempest::Vec3& fromCm, const Tempest::Vec3& toCm,
                      const Tempest::Vec3& velocityKickMS, bool arrow);
    size_t impulse(const Tempest::Vec3& centerCm, float radiusCm, const Tempest::Vec3& velocityKickMS);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    void prepareFrame(uint64_t dt);
    void prepareStep(float seconds,uint32_t nativeCollisionSteps = 1);
    void finishStep();
    void finishFrame();
    friend class CollisionWorld;
    friend class ClothContractDriver;
  };
