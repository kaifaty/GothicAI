#pragma once

#include <Tempest/Vec>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

// The shade of the lids over an eyeball (reports/content/2026-11-hero-head/eye-shader.md, "Способ" 3): how much of
// the light the lids leave to a point of the ball, by how far it lies from the edge of the upper and of the lower lid.
// The edges are those of the face as deformed now (shapes, blinks), found on the ball itself: the lids' triangles
// are seen from the ball's centre, and in each column of yaw the open gap between them is the eye's opening. The
// ball turns under the lids up to 35 degrees, so a map in its UV cannot hold this; the face rig asks it per vertex.
// Angles in the frame of the head bone around the ball's centre: yaw to +y (across), pitch to +x (up), 0 forward (z).
class LidShade final {
  public:
    struct Params final {
      float upperCm   = 0, lowerCm   = 0;  // the shade fades out over this far from the edge of the upper, lower lid
      float upperEdge = 0, lowerEdge = 0;  // the share of the light left at the lid's edge and under it
      };

    static constexpr int   Columns = 71;            // of yaw...
    static constexpr float From    = -70.f;         // ...from this, degrees...
    static constexpr float Step    = 2.f;           // ...in these steps
    static constexpr float Reach   = 80.f;          // the lids are looked for this far up and down from forward
    static constexpr float Band    = 0.25f;         // the lids' triangles are those this near the ball, of its radius
    static constexpr float StillCm = 0.01f;         // lids moved less than this keep their edges (speech barely moves them)

    // The lids around a ball: their vertices (logical ids) and their triangles as triplets of indices into them
    struct Lids final {
      std::vector<uint32_t> verts;
      std::vector<uint32_t> corners;
      bool empty() const { return corners.empty(); }
      };
    // the lids of a part around a ball: the part's triangles with every corner within Band of the radius from the
    // ball's surface and less than Reach degrees from its forward. Their band along the ball makes the opening's
    // edge; the outer skin of the lids stands further off and lies beyond the edge as seen from the centre
    static Lids lids(const std::vector<std::array<uint32_t,3>>& partTriangles, const std::vector<Tempest::Vec3>& pos,
                     const Tempest::Vec3& center, float radius);

    // the edges of the opening now, from the lids at the positions given; false: no lid moved StillCm since the edges
    // were found, they stay
    bool  margins(const std::vector<Tempest::Vec3>& pos, const Lids& lids, const Tempest::Vec3& center);
    bool  hasMargins() const { return valid; }
    // the share of the light the lids leave to a point of the ball (`at` from its centre), 0..1
    float light(const Tempest::Vec3& at, float radius, const Params& p) const;
    // the opening in a column, radians of pitch: lower, upper (equal: the lids meet there)
    std::pair<float,float> opening(int column) const { return {lower[size_t(column)],upper[size_t(column)]}; }

  private:
    std::array<float,Columns> lower{}, upper{};
    std::array<std::vector<std::pair<float,float>>,Columns> spans;  // the lids' cover per column, scratch
    std::vector<float> across, rise;                                  // the lids' vertices seen from the centre, scratch
    std::vector<Tempest::Vec3> seen;                                  // the lids' vertices the edges were found at
    bool valid = false;
  };
