#pragma once

#include "treenaniteasset.h"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace TreeNanite::Pilot {

using Point = ClusterLod::Point;

// Storage and uniform buffer layouts shared with the pilot shaders.
struct Vertex {
  std::array<float, 4> position, normal, uv;
  };

struct Meshlet {
  uint32_t firstVertexWord, firstPrimitiveWord, vertexCount, primitiveCount;
  };

struct DrawRecord {
  uint32_t meshletIndex, instanceIndex, material, flags;
  };

struct Instance {
  std::array<float, 4> translationScale, previousTranslationScale;
  };

struct Frame {
  std::array<float, 16> viewProj, prevViewProj;
  std::array<float, 4>  viewport;
  std::array<uint32_t, 4> control;
  };

struct Material {
  std::array<float, 4> uvScaleFlags, tint;
  };

struct RouteInput {
  Point eye, target;
  float time, pressure, windX, windZ;
  Point sun;
  float threshold;
  };

struct RouteUniform {
  std::array<float, 16> shadowProject;
  std::array<float, 4> eye, right, up, forward, sun, wind, previousWind;
  std::array<float, 4> windFade, previousWindFade;
  };

struct Pose {
  std::array<float, 4> x, y, z;
  };

struct RigBone {
  Point pivot;
  uint32_t parent;
  Point axis;
  uint32_t kind;
  };

static_assert(sizeof(Vertex) == 48 && sizeof(Meshlet) == 16 && sizeof(DrawRecord) == 16);
static_assert(sizeof(Instance) == 32 && sizeof(Frame) == 160 && sizeof(Material) == 32);
static_assert(sizeof(RouteUniform) == 208 && sizeof(Pose) == 48 && sizeof(RigBone) == 32);

struct Phase {
  uint32_t first, count;
  };

struct Submission {
  std::vector<DrawRecord> draws;
  std::vector<Instance>   instances;
  std::vector<Phase>      phases;
  Frame                   frame;
  uint64_t                submittedPrimitives = 0;
  uint64_t                selectedBeforeCull = 0;
  uint64_t                selectedPrimitivesBeforeCull = 0;
  uint64_t                culledMeshlets = 0;
  uint64_t                selectedClusters = 0, terminalClusters = 0;
  std::vector<uint32_t>    requiredDrawRecords; // one-based owner IDs that must win somewhere
  uint32_t                expectedCenterOwner = 0;
  uint32_t                requiredSideBits = 0;
  bool                    requireEdgePixel = false;
  };

struct RouteFrame {
  Submission view, shadow;
  RouteUniform uniform;
  };

class Scene {
  public:
    Scene(const TreeNanite::Asset* asset, uint32_t width = 1280, uint32_t height = 720)
      : asset(asset), width(width), height(height) {
      if(width < 4 || height < 4)
        throw std::runtime_error("tree pilot extent must be at least four pixels in each axis");

      constexpr uint32_t maps = 0x10u | 0x20u | 0x40u;
      constexpr uint32_t crown = 0x8000u | 0x400000u | 0x800000u;
      materials = {{{1.f, 1.f, std::bit_cast<float>(maps), 0.f}, {1.f, 1.f, 1.f, 1.f}},
                   {{1.f, 1.f, std::bit_cast<float>(maps | crown), 0.35f}, {1.f, 1.f, 1.f, 1.f}}};
      if(asset != nullptr)
        importAsset();

      const auto view = camera({}, {0.f, 0.f, 1.f});
      tieMesh = triangle({Point{-100.f, -75.f, 250.f}, Point{110.f, -72.f, 250.f}, Point{-7.f, 104.f, 250.f}});
      nearMesh = triangle({Point{-123.f, -80.f, 190.f}, Point{104.f, -40.f, 142.f}, Point{5.f, 32.f, 20.f}});
      eyeMesh = triangle({Point{-123.f, -80.f, 190.f}, Point{104.f, -40.f, 142.f}, Point{12.f, 32.f, -15.f}});
      frontMesh = triangle({Point{-123.f, -55.f, 240.f}, Point{-20.f, -55.f, 240.f}, Point{-65.f, 64.f, 240.f}});
      backMesh = triangle({Point{14.f, -55.f, 240.f}, Point{70.f, 64.f, 240.f}, Point{119.f, -55.f, 240.f}});

      const Point leftBottom{-90.f, -73.f, 230.f}, rightBottom{90.f, -73.f, 230.f};
      const Point leftTop{-90.f, 73.f, 280.f}, rightTop{90.f, 73.f, 280.f};
      seamLeft = triangle({leftBottom, rightBottom, leftTop});
      seamRight = triangle({rightBottom, rightTop, leftTop}, {{{4.f, 1.f}, {6.f, -2.f}, {-1.f, -3.f}}},
                            {0.5f, 0.15f, -0.85f});

      const float centerX = float(width / 2u) + 0.5f, centerY = float(height / 2u) + 0.5f;
      subpixelMesh = triangle({fromPixel(view, centerX - 0.43f, centerY - 0.19f, 237.f),
                               fromPixel(view, centerX + 0.38f, centerY - 0.21f, 241.f),
                               fromPixel(view, centerX, centerY + 0.39f, 238.f)});
      if(asset == nullptr) {
        denseMesh = triangle({fromPixel(view, -10.f, -10.f, 300.f),
                              fromPixel(view, float(width) * 2.f + 10.f, -10.f, 300.f),
                              fromPixel(view, -10.f, float(height) * 2.f + 10.f, 300.f)});
        sparseMesh = triangle({fromPixel(view, centerX - 30.f, centerY + 25.f, 250.f),
                               fromPixel(view, centerX + 30.f, centerY + 25.f, 240.f),
                               fromPixel(view, centerX, centerY - 40.f, 245.f)});
        }
      }

    static constexpr std::array<std::string_view, 9> testNames() {
      return {"asset", "ties-single", "ties-forward", "ties-reversed", "near-plane", "near-eye-plane",
              "two-sided", "uv-seam", "subpixel"};
      }

    Submission prepare(std::string_view name) const {
      const bool dense = name == "dense" || name == "dense-high";
      const bool sparse = name == "sparse" || name == "asset";
      const auto view = dense ? camera({0.f, 165.f, 0.f}, {0.18f, 0.03f, 1.f}) :
           (sparse && asset != nullptr ? camera({0.f, 170.f, 0.f}, {0.f, 0.11f, 1.f}) : camera({}, {0.f, 0.f, 1.f}));
      Submission submission;
      submission.frame = frame(view);
      if(dense || sparse) {
        if(asset != nullptr) {
          if(dense) {
            submission.instances.reserve(32u * 32u);
            for(int row = 0; row < 32; ++row)
              for(int column = 0; column < 32; ++column)
                appendTree(submission, view, {(float(column) - 15.5f) * 660.f, 0.f, (float(row) + 0.5f) * 660.f});
            } else {
            appendTree(submission, view, {0.f, 0.f, 6500.f});
            }
          } else {
          appendTriangle(submission, dense ? denseMesh : sparseMesh, 1u);
          }

        if(name == "dense-high") {
          // Submit the extra records after the complete base list. Strict Less preserves every base winner.
          const auto count = submission.draws.size();
          submission.draws.reserve(count * 2u);
          for(size_t i = 0; i < count; ++i) {
            const auto record = submission.draws[i];
            submission.draws.push_back(record);
            }
          submission.submittedPrimitives *= 2u;
          }
        }
      else if(name == "ties-single" || name == "ties-forward" || name == "ties-reversed") {
        appendTriangle(submission, tieMesh, 0u);
        appendTriangle(submission, tieMesh, 1u);
        const bool reverse = name == "ties-reversed";
        submission.expectedCenterOwner = reverse ? 2u : 1u;
        submission.requiredDrawRecords = {submission.expectedCenterOwner};
        if(name != "ties-single")
          submission.phases = {{reverse ? 1u : 0u, 1u}, {reverse ? 0u : 1u, 1u}};
        }
      else if(name == "near-plane" || name == "near-eye-plane") {
        appendTriangle(submission, name == "near-plane" ? nearMesh : eyeMesh, 1u);
        submission.requiredDrawRecords = {1u};
        }
      else if(name == "two-sided" || name == "uv-seam") {
        const bool seam = name == "uv-seam";
        appendTriangle(submission, seam ? seamLeft : frontMesh, 1u);
        appendTriangle(submission, seam ? seamRight : backMesh, 1u);
        submission.requiredDrawRecords = {1u, 2u};
        submission.requiredSideBits = seam ? 0u : 3u;
        }
      else if(name == "subpixel") {
        appendTriangle(submission, subpixelMesh, 1u);
        submission.expectedCenterOwner = 1u;
        submission.requiredDrawRecords = {1u};
        submission.requireEdgePixel = true;
        }
      else if(name != "empty") {
        throw std::runtime_error("unknown tree pilot scene");
        }

      if(submission.phases.empty() && !submission.draws.empty())
        submission.phases = {{0u, uint32_t(submission.draws.size())}};
      return submission;
      }

    RouteFrame prepareRoute(const RouteInput& input, const RouteInput& previous, bool cards) const {
      RouteFrame route{};
      const auto packed = [](Point p) { return std::array{p[0], p[1], p[2], 0.f}; };
      const auto view = camera(input.eye, add(input.target, multiply(input.eye, -1.f)));
      const auto last = camera(previous.eye, add(previous.target, multiply(previous.eye, -1.f)));
      const auto sun = normalize(input.sun);
      route.view.frame = frame(view);
      route.view.frame.prevViewProj = frame(last).viewProj;
      route.view.frame.control[2] = uint32_t(asset->bones.size());
      route.view.frame.control[3] = cards ? 1u : 0u;
      route.uniform.shadowProject = shadowProject(sun);
      route.uniform.eye = packed(input.eye);
      route.uniform.right = packed(view.right);
      route.uniform.right[3] = view.xProjection;
      route.uniform.up = packed(view.up);
      route.uniform.up[3] = view.cut.verticalProjection;
      route.uniform.forward = packed(view.cut.forward);
      route.uniform.sun = packed(sun);
      route.uniform.wind = {input.time, input.pressure, input.windX, input.windZ};
      route.uniform.previousWind = {previous.time, previous.pressure, previous.windX, previous.windZ};
      route.uniform.windFade = windFade(view);
      route.uniform.previousWindFade = windFade(last);
      appendTree(route.view, view, {}, cards ? 0.f : input.threshold);
      route.view.instances[0].previousTranslationScale = route.view.instances[0].translationScale;
      route.view.phases = {{0u, uint32_t(route.view.draws.size())}};

      // The 3D caster stays on one fixed cut across the visual LOD transition.
      route.shadow.frame = route.view.frame;
      route.shadow.frame.viewProj = route.uniform.shadowProject;
      appendTree(route.shadow, view, {}, 0.f, false);
      route.shadow.instances[0].previousTranslationScale = route.shadow.instances[0].translationScale;
      route.shadow.phases = {{0u, uint32_t(route.shadow.draws.size())}};
      return route;
      }

    std::vector<Vertex>   vertices;
    std::vector<Meshlet>  meshlets;
    std::vector<uint32_t> words;
    std::vector<Material> materials;
    std::vector<RigBone>  bones;
    std::vector<Skin>     skin;

  private:
    struct Camera {
      TreeNanite::View cut;
      Point right, up;
      float xProjection;
      };

    static Point multiply(Point point, float scale) {
      return {point[0] * scale, point[1] * scale, point[2] * scale};
      }

    static Point add(Point a, Point b) {
      return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
      }

    static Point cross(Point a, Point b) {
      return {a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]};
      }

    static float dot(Point a, Point b) {
      return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
      }

    static Point normalize(Point point) {
      return multiply(point, 1.f / std::sqrt(dot(point, point)));
      }

    Camera camera(Point eye, Point forward) const {
      constexpr float projection = 1.4966058f; // cotangent of half the engine's default 67.5-degree FOV
      Camera view{};
      view.cut.camera = eye;
      view.cut.forward = normalize(forward);
      view.cut.viewportHeight = float(height);
      view.cut.verticalProjection = projection;
      view.cut.nearDepth = 50.f;
      view.cut.farDepth = 24000.f;
      view.xProjection = projection * float(height) / float(width);
      const auto vertical = std::abs(view.cut.forward[1]) > 0.99999f ? Point{0, 0, 1} : Point{0, 1, 0};
      view.right = normalize(cross(vertical, view.cut.forward));
      view.up = cross(view.cut.forward, view.right);

      const auto plane = [eye](Point normal, float distance = 0.f) {
        return TreeNanite::Plane{normal, distance - dot(normal, eye)};
        };
      const Point f = view.cut.forward;
      view.cut.frustum = {plane(add(f, multiply(view.right, view.xProjection))),
                          plane(add(f, multiply(view.right, -view.xProjection))),
                          plane(add(f, multiply(view.up, projection))),
                          plane(add(f, multiply(view.up, -projection))),
                          plane(f, -view.cut.nearDepth), plane(multiply(f, -1.f), view.cut.farDepth)};
      return view;
      }

    Frame frame(const Camera& view) const {
      Frame result{};
      const float nearDepth = view.cut.nearDepth, farDepth = view.cut.farDepth;
      const auto putRow = [&](size_t row, Point normal, float offset = 0.f) {
        for(size_t col = 0; col < 3; ++col)
          result.viewProj[col * 4 + row] = normal[col];
        result.viewProj[12 + row] = offset - dot(normal, view.cut.camera);
        };
      const float zScale = farDepth / (farDepth - nearDepth);
      putRow(0, multiply(view.right, view.xProjection));
      putRow(1, multiply(view.up, -view.cut.verticalProjection));
      putRow(2, multiply(view.cut.forward, zScale), -nearDepth * zScale);
      putRow(3, view.cut.forward);
      result.prevViewProj = result.viewProj;
      result.viewport = {float(width), float(height), 1.f / float(width), 1.f / float(height)};
      return result;
      }

    static std::array<float, 16> shadowProject(Point sun) {
      const Point center{0.f, 650.f, 0.f};
      const auto view = multiply(sun, -1.f);
      const auto vertical = std::abs(view[1]) > 0.99999f ? Point{0, 0, 1} : Point{0, 1, 0};
      const auto right = normalize(cross(vertical, view));
      const auto up = cross(view, right);
      const auto eye = add(center, multiply(sun, 2500.f));
      std::array<float, 16> matrix{};
      const auto putRow = [&](size_t row, Point axis, float extent) {
        for(size_t i = 0; i < 3; ++i)
          matrix[i * 4 + row] = axis[i] / extent;
        matrix[12 + row] = -dot(axis, eye) / extent;
        };
      putRow(0, right, 1550.f);
      putRow(1, up, -1100.f);
      putRow(2, view, 28000.f);
      matrix[15] = 1.f;
      return matrix;
      }

    std::array<float, 4> windFade(const Camera& view) const {
      const auto relative = add(view.cut.camera, {0.f, -heightCm * 0.5f, 0.f});
      const float pixelsPerCm = 0.5f * float(height) * view.cut.verticalProjection /
                               std::max(1.f, std::sqrt(dot(relative, relative)) - heightCm * 0.5f);
      const auto smooth = [](float from, float full, float value) {
        const auto t = std::clamp((value - from) / (full - from), 0.f, 1.f);
        return t*t*(3.f - 2.f*t);
        };
      const float shoot = smooth(0.5f, 0.8f, pixelsPerCm * 2.f);
      const float branch = std::max(shoot, smooth(3.f, 5.f, pixelsPerCm * 0.024f * heightCm));
      const float trunk = std::max(branch, smooth(2.5f, 4.f, pixelsPerCm * 0.03f * heightCm));
      return {trunk, branch, shoot, heightCm};
      }

    Point fromPixel(const Camera& view, float x, float y, float depth) const {
      return {(2.f * x / float(width) - 1.f) * depth / view.xProjection,
              (1.f - 2.f * y / float(height)) * depth / view.cut.verticalProjection, depth};
      }

    void importAsset() {
      vertices.reserve(asset->vertices.size());
      for(const auto& v : asset->vertices) {
        const float openness = float(v.color >> 24) / 255.f;
        vertices.push_back({{v.position[0], v.position[1], v.position[2], 1.f},
                            {v.normal[0], v.normal[1], v.normal[2], openness}, {v.uv[0], v.uv[1], 0.f, 0.f}});
        heightCm = std::max(heightCm, v.position[1]);
        }
      float topTrunkPivot = 0.f;
      for(const auto& bone : asset->bones) {
        bones.push_back({bone.pivot, bone.parent, bone.axis, bone.kind});
        if(bone.kind == TrunkBone)
          topTrunkPivot = std::max(topTrunkPivot, bone.pivot[1]);
        }
      // The fir rig places its fourth trunk bone at 75% of the source height.
      // Derive the wind rate from that common rig, regardless of crown contour.
      if(topTrunkPivot > 0.f)
        heightCm = topTrunkPivot / 0.75f;
      skin = asset->skin;

      words = asset->vertexIds;
      const auto primitiveOffset = uint32_t(words.size());
      words.insert(words.end(), asset->primitives.begin(), asset->primitives.end());
      meshlets.reserve(asset->meshlets.size());
      for(const auto& meshlet : asset->meshlets)
        meshlets.push_back({meshlet.firstVertex, primitiveOffset + meshlet.firstPrimitive,
                            meshlet.vertexCount, meshlet.primitiveCount});
      for(const auto& cluster : asset->clusters) {
        if(cluster.material >= materials.size())
          throw std::runtime_error("tree pilot expects bark and foliage material layers");
        }
      }

    uint32_t triangle(const std::array<Point, 3>& position,
                      const std::array<std::array<float, 2>, 3>& uv = {{{-1.2f, 1.8f}, {2.1f, 1.2f}, {0.2f, -1.7f}}},
                      Point normal = {0.1f, 0.15f, -1.f}) {
      const std::array<Point, 3> normals = {normalize(normal), normalize(add(normal, {-0.12f, 0.35f, 0.05f})),
                                            normalize(add(normal, {0.37f, -0.11f, 0.04f}))};
      const auto first = uint32_t(words.size());
      for(size_t i = 0; i < 3; ++i) {
        words.push_back(uint32_t(vertices.size()));
        vertices.push_back({{position[i][0], position[i][1], position[i][2], 1.f},
                            {normals[i][0], normals[i][1], normals[i][2], 0.65f + 0.16f * float(i)},
                            {uv[i][0], uv[i][1], 0.f, 0.f}});
        }
      words.push_back(0x00020100u);
      meshlets.push_back({first, first + 3u, 3u, 1u});
      return uint32_t(meshlets.size() - 1u);
      }

    static uint32_t addInstance(Submission& submission, Point position) {
      submission.instances.push_back({{position[0], position[1], position[2], 1.f},
                                      {position[0] - 1.3f, position[1] + 0.8f, position[2] - 0.3f, 1.f}});
      return uint32_t(submission.instances.size() - 1u);
      }

    void appendTriangle(Submission& submission, uint32_t meshlet, uint32_t material) const {
      submission.draws.push_back({meshlet, addInstance(submission, {}), material, 0u});
      ++submission.submittedPrimitives;
      ++submission.selectedBeforeCull;
      ++submission.selectedPrimitivesBeforeCull;
      }

    void appendTree(Submission& submission, const Camera& view, Point position, float threshold = 1.f,
                    bool cull = true) const {
      const uint32_t instance = addInstance(submission, position);
      for(const auto& cluster : asset->clusters) {
        if(!TreeNanite::clusterSelected(*asset, cluster, view.cut, position, 1.f, threshold))
          continue;
        ++submission.selectedClusters;
        submission.terminalClusters += std::isinf(asset->groups[cluster.parentGroup].errorCm);
        submission.selectedBeforeCull += cluster.meshletCount;
        const uint32_t end = cluster.firstMeshlet + cluster.meshletCount;
        for(uint32_t index = cluster.firstMeshlet; index < end; ++index) {
          const auto& meshlet = asset->meshlets[index];
          submission.selectedPrimitivesBeforeCull += meshlet.primitiveCount;
          if(cull && !TreeNanite::meshletVisible(meshlet, view.cut, position, 1.f)) {
            ++submission.culledMeshlets;
            continue;
            }
          submission.draws.push_back({index, instance, cluster.material, 0u});
          submission.submittedPrimitives += meshlet.primitiveCount;
          }
        }
      }

    const TreeNanite::Asset* asset;
    uint32_t width, height;
    float heightCm = 1.f;
    uint32_t tieMesh, nearMesh, eyeMesh, frontMesh, backMesh, seamLeft, seamRight, subpixelMesh;
    uint32_t denseMesh = 0, sparseMesh = 0;
  };

}
