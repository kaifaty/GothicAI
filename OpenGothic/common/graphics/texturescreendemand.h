#pragma once

#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "graphics/mesh/texeldensity.h"
#include "texturelevels.h"

// Resolution demands of texture owners by their distance (reports/engine/2026-11-10-texture-mips/README.md).
// An owner's exponent (texturelevels.h) holds while its distance to the hero and the camera stays within the margin it
// was evaluated with. Instead of walking the owners every frame, each one waits in a queue keyed by the reading of the
// view odometer (ChunkResidency::viewOdometer) at which that margin runs out; a frame evaluates only the owners due.
class TextureScreenDemand final {
  public:
    // Reserved exponents: the whole chain, for an owner measured without a usable density (collapsed UVs); the
    // coarsest allowed level, for one with nothing to draw (no triangle with any world area).
    static constexpr int32_t WholeChain = std::numeric_limits<int32_t>::max();
    static constexpr int32_t Coarsest   = std::numeric_limits<int32_t>::min()+1;

    struct View {
      Tempest::Vec3 hero, camera;  // centimetres
      double        odometer = 0;  // metres
      bool          known    = false;
      int32_t       coarsest = std::numeric_limits<int32_t>::min(); // TextureLevels::coarsest of the reserve
      // The class adds its margin only while the stages use it (stage 2 and deeper); otherwise an owner is due by its
      // exponent's margin alone, and its class is brought up to date only when it is evaluated anyway.
      bool          classMargin = false;
      // Step 4, the look-ahead (texturelookahead.h): where the hero and the camera will be within the horizon, as
      // offsets in centimetres from where they are (ChunkResidency::viewHeroAhead). An owner's exponent comes from the
      // closest point of each path; zero offsets: from the points, as before.
      Tempest::Vec3 heroAhead, cameraAhead;
      bool lookAhead() const;
      };
    // The distance class an owner with an exponent registers in place of its chunk ring (TextureNeed::ring), for the
    // stages above the memory target (texturebudget.h): 0 nearer than one chunk, 1 nearer than two, 2 farther, from
    // the same distance as its exponent, so it means what ring 0 / 1 / 2+ meant and never waits for a chunk change.
    static constexpr uint32_t NoClass         = uint32_t(-1);
    static constexpr double   ClassBorder[2]  = {64.0, 128.0};  // metres: one and two ChunkId::Size
    // Moving away, an owner keeps its nearer class until its distance passes the border by this part of it: it errs
    // sharper, and a view at a border does not re-register the owner on every step.
    static constexpr double   ClassHysteresis = 0.125;

    struct Result {
      int32_t  exponent = TextureLevels::NoExponent;
      double   expiry   = 0;         // the odometer reading to evaluate again at
      double   margin   = 0;         // metres the distance may change before the exponent or the class does
      bool     scheduled = false;    // false: the demand does not change with distance
      uint32_t distanceClass = NoClass;  // with an exponent from a distance: the owner's distance class
      // The exponent at the present distance, without the look-ahead: what the screen needs now, for the shortfall
      // (Resources measures it against this). Equal to `exponent` while the view stands still or the look-ahead is off.
      int32_t  present  = TextureLevels::NoExponent;
      };
    // The class at `distance` metres, `previous` the owner's class so far (or NoClass); `margin` gets the metres the
    // distance may change before the class does.
    static uint32_t distanceClass(double distance, uint32_t previous, double& margin);

    // The bounds of an owner placed in the world by a position only (an NPC, an item): a sphere around it,
    // centimetres. It holds the mesh in any rotation and, for a skinned mesh, in its bind pose.
    struct Sphere {
      Tempest::Vec3 centre;
      float         radius = 0;
      };
    // The largest reach an owner placed by a position gets (VisualObjects::textureAllowance), centimetres: its sphere
    // is grown by it, so it errs sharper by at most this.
    static constexpr float MaxReach = 400;

    // The demand of an owner with `texel` in world space (bounds in centimetres) and `previous` exponent: its exponent
    // from the nearer of the hero and the camera, due again when either may have come or gone by its margin. Without a
    // view: the ring rule, due as soon as there is one. Without a density: the whole chain; with nothing to draw: the
    // coarsest allowed level.
    // With the look-ahead the exponent is from the closest point of the view's paths, `present` from the points; the
    // owner is due again when either may change. `previousPresent` holds the present one as `previous` does the other.
    static Result evaluate(const View& view, const TexelDensity& texel, int32_t previous = TextureLevels::NoExponent,
                           uint32_t previousClass = NoClass, int32_t previousPresent = TextureLevels::NoExponent);
    // The same with the bounds a sphere; only the density and flatness of `texel` count.
    static Result evaluate(const View& view, const TexelDensity& texel, const Sphere& bounds,
                           int32_t previous = TextureLevels::NoExponent, uint32_t previousClass = NoClass,
                           int32_t previousPresent = TextureLevels::NoExponent);
    // Distance in metres from `point` (centimetres) to the bounds of `texel`, or to a sphere.
    static double distance(const Tempest::Vec3& point, const TexelDensity& texel);
    static double distance(const Tempest::Vec3& point, const Sphere& sphere);

    // A min-heap without removal: an entry carries the owner's stamp of its scheduling, and one whose owner was
    // scheduled again or released since is dropped when it comes up.
    struct Entry {
      double   expiry = 0;
      size_t   owner  = 0;
      uint64_t stamp  = 0;
      };
    void   push(size_t owner, double expiry, uint64_t stamp);
    // The earliest entry if it is due at `odometer`.
    bool   due(double odometer, Entry& out) const;
    void   pop();
    size_t size() const { return heap.size(); }
    // Keeps only the entries `live(entry)` accepts.
    template<class F>
    void   compact(F live);

  private:
    void   rebuild();
    std::vector<Entry> heap;
  };

template<class F>
void TextureScreenDemand::compact(F live) {
  std::vector<Entry> kept;
  kept.reserve(heap.size()/2);
  for(const auto& entry:heap)
    if(live(entry))
      kept.push_back(entry);
  heap = std::move(kept);
  rebuild();
  }
