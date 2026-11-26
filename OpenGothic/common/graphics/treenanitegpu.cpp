#include "treenanitegpu.h"

#include "baseidentity.h"
#include "commandline.h"
#include "foliagetree.h"
#include "graphics/mesh/treenaniteasset.h"
#include "graphics/mesh/treenanitecontours.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "resources.h"
#include "shaders.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace Tempest;

namespace TreeNanite {
namespace {

struct GpuVertex {
  Vec4 position, normal, uv;
  };

struct GpuSource {
  Vec4 lower, upper;
  std::array<uint32_t, 4> data;
  };

struct GpuGroup {
  Vec4 sphere, error;
  };

struct GpuBone {
  Point pivot;
  uint32_t parent;
  Point axis;
  uint32_t kind;
  };

using GpuMeshlet = std::array<uint32_t, 4>;
using GpuShape = std::array<uint32_t, 4>;

static_assert(sizeof(GpuVertex) == 48 && sizeof(GpuSource) == 48 && sizeof(GpuGroup) == 32);
static_assert(sizeof(GpuMeshlet) == 16 && sizeof(GpuShape) == 16 && sizeof(GpuBone) == 32);

constexpr uint32_t ContourFlag = 1u << 31;
constexpr uint32_t FixedShadowMip = (7u * Contours::MipSteps) - Contours::MipBase;

Vec4 vector(const Point& point, float w = 0.f) {
  return {point[0], point[1], point[2], w};
  }

std::string hashString(const std::array<uint8_t, 32>& hash) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(hash.size()*2);
  for(const uint8_t byte : hash) {
    result += hex[byte >> 4];
    result += hex[byte & 15u];
    }
  return result;
  }

struct Pack {
  std::vector<GpuVertex> vertices;
  std::vector<GpuMeshlet> meshlets;
  std::vector<uint32_t> words;
  std::vector<Skin> skin;
  std::vector<GpuSource> source, faces, shadow;
  std::vector<GpuGroup> groups;
  std::vector<GpuShape> shapes;
  std::vector<GpuBone> bones;
  };

void packDAG(Pack& pack, const Asset& asset, uint32_t level) {
  const auto firstVertex = uint32_t(pack.vertices.size());
  for(const auto& v : asset.vertices)
    pack.vertices.push_back({vector(v.position, 1.f), vector(v.normal, float(v.color >> 24) / 255.f),
                             {v.uv[0], v.uv[1], 0.f, 0.f}});
  pack.skin.insert(pack.skin.end(), asset.skin.begin(), asset.skin.end());

  const auto firstVertexWord = uint32_t(pack.words.size());
  for(const uint32_t id : asset.vertexIds)
    pack.words.push_back(firstVertex + id);
  const auto firstPrimitiveWord = uint32_t(pack.words.size());
  pack.words.insert(pack.words.end(), asset.primitives.begin(), asset.primitives.end());

  const uint32_t firstGroup = uint32_t(pack.groups.size());
  for(const auto& g : asset.groups)
    pack.groups.push_back({vector(g.bounds.center, g.bounds.radius), {g.errorCm, 0.f, 0.f, 0.f}});

  for(const auto& c : asset.clusters) {
    if(c.material > 1)
      throw std::runtime_error("expected bark and crown in the tree DAG");
    // The four authored distant levels use the same bark as the accepted route.
    // Needles are separately selected from their UV footprint contour shapes.
    if(level != 0 && (c.material != 0 || c.refinedGroup != NoGroup))
      continue;
    const uint32_t refined = c.refinedGroup == NoGroup ? NoGroup : firstGroup + c.refinedGroup;
    const uint32_t parent = firstGroup + c.parentGroup;
    for(uint32_t m = c.firstMeshlet; m < c.firstMeshlet+c.meshletCount; ++m) {
      const auto& item = asset.meshlets[m];
      const auto raster = uint32_t(pack.meshlets.size());
      pack.meshlets.push_back({firstVertexWord + item.firstVertex, firstPrimitiveWord + item.firstPrimitive,
                              item.vertexCount, item.primitiveCount});
      const auto lower = vector(item.boxMin), upper = vector(item.boxMax);
      pack.source.push_back({lower, upper, {refined, parent, c.material, level}});
      if(level == 4)
        pack.shadow.push_back({lower, upper, {raster, c.material, 0, 0}});
      }
    }
  }

void packContourFaces(Pack& pack, const Asset& level, const Contours& contours, uint32_t levelIndex,
                      uint32_t firstVertex) {
  const auto& faces = contours.faces[levelIndex - 1];
  for(const auto& face : faces) {
    const auto operand = uint32_t(pack.words.size());
    if(operand >= (ContourFlag >> 3))
      throw std::runtime_error("tree contour operand exceeds the record field");
    Point lower, upper;
    lower.fill(std::numeric_limits<float>::infinity());
    upper.fill(-std::numeric_limits<float>::infinity());
    for(const uint32_t vertex : face.vertices) {
      if(vertex >= level.vertices.size())
        throw std::runtime_error("tree contour vertex is outside the authored level");
      pack.words.push_back(firstVertex + vertex);
      const auto& position = level.vertices[vertex].position;
      const float travel = windTravel(level, vertex);
      for(size_t a = 0; a < position.size(); ++a) {
        lower[a] = std::min(lower[a], position[a] - travel);
        upper[a] = std::max(upper[a], position[a] + travel);
        }
      }
    const auto chart = face.chart * Contours::MipCount;
    pack.faces.push_back({vector(lower), vector(upper), {operand, chart, levelIndex, 0}});
    }
  }

void packContours(Pack& pack, const Contours& contours, uint32_t lastLevelStart) {
  const auto rasterBase = uint32_t(pack.meshlets.size());
  const auto vertexBase = uint32_t(pack.words.size());
  pack.words.insert(pack.words.end(), contours.barycentrics.begin(), contours.barycentrics.end());
  const auto primitiveBase = uint32_t(pack.words.size());
  pack.words.insert(pack.words.end(), contours.primitives.begin(), contours.primitives.end());
  for(const auto& meshlet : contours.meshlets)
    pack.meshlets.push_back({vertexBase + meshlet.firstVertex, primitiveBase + meshlet.firstPrimitive,
                            meshlet.vertexCount, meshlet.primitiveCount});
  for(const auto& shape : contours.shapes) {
    // A task payload holds up to 52 meshlets for each of its 32 faces.
    if(shape.meshletCount > 52)
      throw std::runtime_error("tree contour exceeds the mesh task payload");
    uint32_t triangles = 0;
    for(uint32_t i = 0; i < shape.meshletCount; ++i)
      triangles += contours.meshlets[shape.firstMeshlet + i].primitiveCount;
    pack.shapes.push_back({rasterBase + shape.firstMeshlet, shape.meshletCount, triangles, 0});
    }

  // Stage 3 keeps one geometric caster across camera levels. Stage 4 owns shadow LOD.
  for(size_t i = lastLevelStart; i < pack.faces.size(); ++i) {
    const auto& face = pack.faces[i];
    const auto& shape = pack.shapes[face.data[1] + FixedShadowMip];
    const uint32_t flags = ContourFlag | (face.data[0] << 3u) | face.data[2];
    for(uint32_t m = 0; m < shape[1]; ++m)
      pack.shadow.push_back({face.lower, face.upper, {shape[0] + m, 1, flags, 0}});
    }
  }

void attachImpl(StaticMesh& mesh, const AssetFiles& files,
                const std::array<std::filesystem::path, 4>& authoredLevels) {
  const auto primary = load(files.dag);
  const auto contours = Contours::load(files.contours);
  if(primary.bones.empty() || mesh.levels.size() != authoredLevels.size() || mesh.impostor.px <= 0.f)
    throw std::runtime_error("expected a skinned tree with four authored levels and an impostor");

  auto gpu = std::make_unique<GpuAsset>();
  constexpr std::array<std::string_view, 2> materials{"OWN_TREE_FIR_BARK.TGA", "OWN_TREE_FIR_BOUGHS.TGA"};
  for(size_t i = 0; i < materials.size(); ++i) {
    const auto sub = std::ranges::find(mesh.sub, materials[i], &StaticMesh::SubMesh::texName);
    if(sub == mesh.sub.end() || sub->material.tex == nullptr || !FoliageTree::isTree(sub->material.foliage))
      throw std::runtime_error("expected authored spruce bark and crown materials with tree wind");
    gpu->materialSub[i] = uint32_t(sub - mesh.sub.begin());
    }
  gpu->registrationSub = gpu->materialSub[1];

  Pack pack;
  packDAG(pack, primary, 0);
  for(const auto& bone : primary.bones) {
    pack.bones.push_back({bone.pivot, bone.parent, bone.axis, bone.kind});
    if(bone.kind == TrunkBone)
      gpu->windHeight = std::max(gpu->windHeight, bone.pivot[1] / 0.75f);
    }
  const auto sameBone = [](const Bone& a, const Bone& b) {
    return a.pivot == b.pivot && a.axis == b.axis && a.parent == b.parent && a.kind == b.kind;
    };
  for(size_t i = 0; i < files.levels.size(); ++i) {
    const auto level = load(files.levels[i]);
    if(level.materialHash != primary.materialHash || !std::ranges::equal(level.bones, primary.bones, sameBone) ||
       hashString(level.sourceHash) != BaseImport::sha256File(authoredLevels[i]))
      throw std::runtime_error("tree level does not match its authored geometry and common rig: " + files.levels[i].string());
    gpu->levelPx[i] = mesh.levels[i].px;
    gpu->contourOffsets[i] = uint32_t(pack.faces.size());
    const auto firstVertex = uint32_t(pack.vertices.size());
    packDAG(pack, level, uint32_t(i+1));
    packContourFaces(pack, level, contours, uint32_t(i+1), firstVertex);
    }
  gpu->contourOffsets.back() = uint32_t(pack.faces.size());
  packContours(pack, contours, gpu->contourOffsets[3]);

  const auto& rest = mesh.bbox.bbox;
  const auto center = (rest[0] + rest[1]) * 0.5f;
  gpu->boundsCenterHeight = {center.x, center.y, center.z, rest[1].y-rest[0].y};
  gpu->impostorPx = mesh.impostor.px;
  Point lower{rest[0].x, rest[0].y, rest[0].z}, upper{rest[1].x, rest[1].y, rest[1].z};
  const auto enclose = [&](const GpuSource& src) {
    lower[0] = std::min(lower[0], src.lower.x);
    lower[1] = std::min(lower[1], src.lower.y);
    lower[2] = std::min(lower[2], src.lower.z);
    upper[0] = std::max(upper[0], src.upper.x);
    upper[1] = std::max(upper[1], src.upper.y);
    upper[2] = std::max(upper[2], src.upper.z);
    };
  std::ranges::for_each(pack.source, enclose);
  std::ranges::for_each(pack.faces, enclose);
  gpu->boundsMin = vector(lower);
  gpu->boundsMax = vector(upper);
  gpu->sourceCount = uint32_t(pack.source.size());
  gpu->meshletCount = uint32_t(pack.meshlets.size());
  gpu->boneCount = uint32_t(pack.bones.size());
  gpu->shadowCount = uint32_t(pack.shadow.size());

  auto& device = Resources::device();
  const auto upload = [&]<class T>(const std::vector<T>& data) {
    if(data.empty() || data.size() > device.properties().ssbo.maxRange / sizeof(T))
      throw std::runtime_error("tree asset storage exceeds the device buffer range");
    return device.ssbo(data);
    };
  gpu->vertices = upload(pack.vertices);
  gpu->meshlets = upload(pack.meshlets);
  gpu->words = upload(pack.words);
  gpu->source = upload(pack.source);
  gpu->groups = upload(pack.groups);
  gpu->skin = upload(pack.skin);
  gpu->bones = upload(pack.bones);
  gpu->contourFaces = upload(pack.faces);
  gpu->contourShapes = upload(pack.shapes);
  gpu->shadow = upload(pack.shadow);
  Log::i("tree-nanite: loaded ", files.dag.string(), ", sources=", gpu->sourceCount,
         ", contour_faces=", gpu->contourOffsets.back(), ", shadow_meshlets=", gpu->shadowCount,
         ", bones=", gpu->boneCount);
  mesh.treeNanite = std::move(gpu);
  }

}

bool enabled() {
#if defined(__APPLE__)
  return false;
#else
  static const bool on = [] {
    const auto* value = std::getenv("OPENGOTHIC_TREE_NANITE");
    if(value == nullptr || std::string_view(value) != "1")
      return false;
#if defined(_MSC_VER)
    if(CommandLine::inst().graphicsApi() == CommandLine::DirectX12)
      return false;
#endif
    const auto& options = Shaders::options();
    const auto& device = Resources::device().properties();
    const size_t attachments = options.doMaterialMaps ? 5 : 4;
    const bool supported = options.doMeshShading && options.doBindless && device.meshlets.taskShader && device.storeAndAtomicVs &&
                           device.render.maxClipCullDistances >= 5 &&
                           device.render.maxColorAttachments >= attachments && device.hasAttachFormat(TextureFormat::RG32U) &&
                           device.hasSamplerFormat(TextureFormat::RG32U) && device.hasStorageFormat(TextureFormat::R32F) &&
                           device.hasSamplerFormat(TextureFormat::R32F);
    if(!supported)
      Log::i("tree-nanite: required Vulkan mesh shading or visibility formats are unavailable");
    return supported;
    }();
  return on;
#endif
  }

void attach(StaticMesh& mesh, const AssetFiles& files,
            const std::array<std::filesystem::path, 4>& authoredLevels) {
  try {
    attachImpl(mesh, files, authoredLevels);
    }
  catch(const std::exception& error) {
    Log::e("tree-nanite: asset ", files.dag.string(), " is unavailable: ", error.what());
    }
  }

}
