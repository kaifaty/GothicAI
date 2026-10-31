#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class Skeleton;

// HitZones: where an arrow hits a body - the head, the trunk, a limb. Capsules along the bones, made from
// the skeleton's bind pose by the bone names (a human, a wolf, a troll alike), tested against the animated
// bones: an arrow that passes between the arm and the trunk misses. Their thickness is fitted to the body's
// mesh (fit): the points around each bone, else a guess by its name and the creature's size
class HitZones final {
  public:
    enum Zone : uint8_t {
      None,
      Body,
      Head,
      Limb,
      };

    void             setSkeleton(const Skeleton* sk);
    bool             isValid() const { return !caps.empty(); }
    // the thickness from the body's shape: points by bone in the bone's frame (a skinned mesh), and the
    // points of a mesh on a bone (a head); nothing: the guess
    struct Shape final {
      const std::vector<std::vector<Tempest::Vec3>>* byNode = nullptr;
      size_t                                         node   = size_t(-1);
      const std::vector<Tempest::Vec3>*              points = nullptr;
      };
    void             fit(const std::vector<Shape>& shape);

    static bool      isFitEnabled();
    static void      setFitEnabled(bool e);

    // The first zone the segment from-to enters; t: how far from 'from'. tr: the world-space bones,
    // scale: of the npc (the radii grow with it)
    Zone             rayTest(const Tempest::Matrix4x4* tr, size_t numBones, float scale,
                             const Tempest::Vec3& from, const Tempest::Vec3& to, float& t,
                             size_t* bone = nullptr) const;
    // Secondary owner lookup for vanilla dice hits, which do not select a zone.
    size_t           closestBone(const Tempest::Matrix4x4* tr, size_t numBones, float scale,
                                 const Tempest::Vec3& at) const;
    // The middle of a zone: the test bench aims there
    bool             center(const Tempest::Matrix4x4* tr, size_t numBones, Zone z, Tempest::Vec3& at) const;

    struct Posed final {
      Tempest::Vec3 a, b;
      float         r    = 0;
      Zone          zone = None;
      size_t        bone = size_t(-1);
      };
    // the capsules as posed (the test bench draws them)
    void             posed(const Tempest::Matrix4x4* tr, size_t numBones, float scale, std::vector<Posed>& out) const;
    // Signed XZ clearance between torso capsules; offset translates the first body.
    static float     horizontalBodyGap(const std::vector<Posed>& a, const std::vector<Posed>& b,
                                      const Tempest::Vec3& offset = {});

    static std::string_view name(Zone z);
    // the capsules as posed, for debugging: (a, b, radius, zone)
    std::string      dump(const Tempest::Matrix4x4* tr, size_t numBones, float scale) const;
    // how well the zones cover the body as posed (the shape fitted to): each zone, its points in it, in any
    std::string      coverage(const Tempest::Matrix4x4* tr, size_t numBones, float scale) const;

  private:
    struct End final {
      size_t        bone = 0;
      Tempest::Vec3 at   = {}; // in the bone's frame
      };
    struct Capsule final {
      End    a, b;
      float  r     = 0;
      float  guess = 0;          // by the bone's name and the creature's size
      Zone   zone  = None;
      size_t owner = size_t(-1); // the bone whose flesh it is
      bool   fitted = false;
      };
    struct Point final {
      size_t        node = 0;
      Tempest::Vec3 at   = {};
      };

    std::vector<Capsule> caps;
    std::vector<Point>   shape; // fitted to, for the coverage
    const Skeleton*      skeleton = nullptr;
    float                size     = 0; // cm: the creature from end to end (the bind pose)

    static Tempest::Vec3 point(const Tempest::Matrix4x4* tr, const End& e);
    friend class Ragdoll;
  };
