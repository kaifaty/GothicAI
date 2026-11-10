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
    // A reserved exponent: the whole chain, for an owner measured without a usable density.
    static constexpr int32_t WholeChain = std::numeric_limits<int32_t>::max();

    struct View {
      Tempest::Vec3 hero, camera;  // centimetres
      double        odometer = 0;  // metres
      bool          known    = false;
      int32_t       coarsest = std::numeric_limits<int32_t>::min(); // TextureLevels::coarsest of the reserve
      };
    struct Result {
      int32_t exponent = TextureLevels::NoExponent;
      double  expiry   = 0;         // the odometer reading to evaluate again at
      bool    scheduled = false;    // false: the demand does not change with distance
      };

    // The demand of an owner with `texel` in world space (bounds in centimetres) and `previous` exponent: its exponent
    // from the nearer of the hero and the camera, due again when either may have come or gone by its margin. Without a
    // view: the ring rule, due as soon as there is one. Without a density: the whole chain.
    static Result evaluate(const View& view, const TexelDensity& texel, int32_t previous = TextureLevels::NoExponent);
    // Distance in metres from `point` (centimetres) to the bounds of `texel`.
    static double distance(const Tempest::Vec3& point, const TexelDensity& texel);

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
