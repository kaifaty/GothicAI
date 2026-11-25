// Included by mesh_gltf_test.cpp.

#include "tree_bough_groups_test.h"
#include "tree_level_band_test.h"

void treeBoughRasterCases(Tempest::Device& device) {
  using namespace Tempest;
  constexpr uint32_t side = 32;
  constexpr uint32_t indicesPerCard = 6;
  const auto shader = [&](std::string_view name) {
    const auto code = GothicShader::get(name);
    return device.shader(code.data, code.len);
    };
  constexpr Resources::Vertex quad[] = {
    {{-0.8f,-0.8f,0.4f},{0.f,0.f,1.f},{0.f,0.f},0xffffffffu},
    {{ 0.8f,-0.8f,0.4f},{0.f,0.f,1.f},{1.f,0.f},0xffffffffu},
    {{ 0.8f, 0.8f,0.4f},{0.f,0.f,1.f},{1.f,1.f},0xffffffffu},
    {{-0.8f, 0.8f,0.4f},{0.f,0.f,1.f},{0.f,1.f},0xffffffffu},
    // three more nodes of a packed mesh workgroup: two cards and a triangle that do not overlap
    {{-0.9f,-0.9f,0.4f},{0.f,0.f,1.f},{0.f,0.f},0xffffffffu},
    {{-0.1f,-0.9f,0.4f},{0.f,0.f,1.f},{1.f,0.f},0xffffffffu},
    {{-0.1f, 0.9f,0.4f},{0.f,0.f,1.f},{1.f,1.f},0xffffffffu},
    {{-0.9f, 0.9f,0.4f},{0.f,0.f,1.f},{0.f,1.f},0xffffffffu},
    {{ 0.1f,-0.9f,0.4f},{0.f,0.f,1.f},{0.f,0.f},0xffffffffu},
    {{ 0.9f,-0.9f,0.4f},{0.f,0.f,1.f},{1.f,0.f},0xffffffffu},
    {{ 0.9f, 0.3f,0.4f},{0.f,0.f,1.f},{1.f,1.f},0xffffffffu},
    {{ 0.1f, 0.3f,0.4f},{0.f,0.f,1.f},{0.f,1.f},0xffffffffu},
    {{ 0.1f, 0.4f,0.4f},{0.f,0.f,1.f},{0.f,0.f},0xffffffffu},
    {{ 0.9f, 0.4f,0.4f},{0.f,0.f,1.f},{1.f,0.f},0xffffffffu},
    {{ 0.5f,0.95f,0.4f},{0.f,0.f,1.f},{1.f,1.f},0xffffffffu},
    };
  std::vector<uint32_t> compact = {4,0,1,2,0,2,3, (4u<<16)|4u,4,5,6,4,6,7, (8u<<16)|4u,8,9,10,8,10,11, (12u<<16)|3u,12,13,14};
  uint32_t packed[PackedMesh::MaxPrim] = {0x020100u,0x030200u};
  packed[PackedMesh::MaxPrim-1] = 0x04020000u; // two triangles and four vertices
  const uint32_t tasks[][4] = {
    {0,0,0,indicesPerCard << 24},
    {0,0,1u << 16,(indicesPerCard << 24) | 0x800000u},
    {0,0,(1u | TreeBough::DescendantSide) << 16,(indicesPerCard << 24) | 0x800000u},
    };
  // The mesh path keeps the vertex count of a node above its first index. After the three tasks above come a steady
  // card, the parent side of a band, the child side of another one and the empty node that pads a workgroup.
  const auto meshTask = [](uint32_t first, uint32_t vertices, uint32_t band, uint32_t indices, uint32_t weight) {
    return std::array<uint32_t,4>{0,first | (vertices << TreeBough::FirstIndexBits),band << 16,(indices << 24) | weight};
    };
  std::vector<std::array<uint32_t,4>> meshTasks = {
    meshTask(0,4,0,indicesPerCard,0), meshTask(0,4,1,indicesPerCard,0x800000u),
    meshTask(0,4,1u | TreeBough::DescendantSide,indicesPerCard,0x800000u),
    meshTask(7,4,0,indicesPerCard,0), meshTask(14,4,2,indicesPerCard,0xc00000u),
    meshTask(21,3,4u | TreeBough::DescendantSide,3,0x400000u), {},
    };
  // Eight cards and two nodes of 32 vertices follow: workgroups above 32 vertices. A card and a wide node are in a band.
  constexpr uint32_t wideCards = 7, wideNode = wideCards+8;
  std::vector<Resources::Vertex> vertices(std::begin(quad), std::end(quad));
  for(const auto [first,count,indices] : treeBoughWideNodes(vertices,compact)) {
    const bool wide = meshTasks.size()==wideNode, banded = meshTasks.size()==wideCards+3;
    meshTasks.push_back(meshTask(first,count,wide ? 4u | TreeBough::DescendantSide : banded ? 2u : 0u,indices,
                                 wide ? 0x400000u : banded ? 0xc00000u : 0u));
    }
  const float instance[16] = {1.f,0.f,0.f, 0.f,1.f,0.f, 0.f,0.f,1.f, 0.f,0.f,0.f};
  uint32_t bucket[36] = {};
  bucket[12] = bucket[15] = std::bit_cast<uint32_t>(1.f); // alphaWeight and uvScale
  bucket[14] = 0x200u | 0x10u | 0x1000000u; // BK_HASHED_ALPHA | BK_NORMAL | BK_CANOPY
  SceneGlobals::UboGlobal globals{};
  globals.view = globals.project = Matrix4x4::mkIdentity();
  globals.project[3][0] = 0.013f;
  globals.viewProject = globals.project;
  globals.clipInfo = {0.1f,0.f,1.f};
  globals.screenRes = {int(side),int(side)};
  globals.screenResInv = {1.f/float(side),1.f/float(side)};
  globals.mipGradScale = 1.f;
  globals.viewFlags = SceneGlobals::VF_TAA;
  globals.taaFrame = 7;

  const auto scene = device.ssbo(&globals, sizeof(globals));
  const auto payload = device.ssbo(tasks, sizeof(tasks));
  const auto meshPayload = device.ssbo(meshTasks);
  const auto instances = device.ssbo(instance, sizeof(instance));
  float jointInstance[16];
  std::ranges::copy(instance, jointInstance);
  jointInstance[13] = std::bit_cast<float>(TreeBough::impostorJointFade(0.8f)); // static draw's animation slot
  const auto jointInstances = device.ssbo(jointInstance, sizeof(jointInstance));
  float coarseInstance[16];
  std::ranges::copy(instance, coarseInstance);
  coarseInstance[13] = std::bit_cast<float>(MeshLevels::CanopyVertexNormal);
  const auto coarseInstances = device.ssbo(coarseInstance, sizeof(coarseInstance));
  coarseInstance[13] = std::bit_cast<float>(TreeBough::impostorJointFade(0.8f) | MeshLevels::CanopyVertexNormal);
  const auto coarseJointInstances = device.ssbo(coarseInstance, sizeof(coarseInstance));
  const auto buckets = device.ssbo(bucket, sizeof(bucket));
  const auto vbo = device.ssbo(vertices);
  const auto compactIbo = device.ssbo(compact);
  const auto packedIbo = device.ssbo(packed, sizeof(packed));
  Pixmap albedo(1,1,TextureFormat::RGBA8);
  const uint8_t texel[] = {255,255,255,191};
  std::memcpy(albedo.data(),texel,sizeof(texel));
  const auto texture = device.texture(albedo,false);
  const uint8_t normalTexel[] = {216,128,240,255};
  std::memcpy(albedo.data(),normalTexel,sizeof(normalTexel));
  const auto detail = device.texture(albedo,false);
  auto diffuse = device.attachment(TextureFormat::RGBA8,side,side);
  auto normal = device.attachment(TextureFormat::R32U,side,side);
  auto orm = device.attachment(TextureFormat::RGBA8,side,side);
  auto motion = device.attachment(TextureFormat::RG16F,side,side);
  RenderState state;
  state.setCullFaceMode(RenderState::CullMode::NoCull);
  state.setZWriteEnabled(false);

  for(bool mesh : {false,true}) {
    if(mesh && !device.properties().meshlets.meshShader)
      continue;
    const auto pipeline = [&](std::string_view vert, std::string_view meshName, std::string_view frag) {
      if(mesh)
        return device.pipeline(state, Shader(), shader(meshName), shader(frag));
      return device.pipeline(Topology::Triangles,state,shader(vert),shader(frag));
      };
    const auto bough = pipeline("main_bough_g_orm_slot.vert.sprv", "main_bough_g_orm_slot.mesh.sprv", "main_bough_g_at_orm_slot.frag.sprv");
    const auto legacy = pipeline("main_obj_g_orm_slot.vert.sprv", "main_obj_g_orm_slot.mesh.sprv", "main_obj_g_at_orm_slot.frag.sprv");
    const auto jointGeometry = pipeline("main_joint_g_orm_slot.vert.sprv", "main_joint_g_orm_slot.mesh.sprv", "main_joint_g_at_orm_slot.frag.sprv");
    // Draws `count` workgroups of `size` nodes each, or one node of the vertex path.
    const auto renderGroups = [&](const RenderPipeline& pso, const StorageBuffer& ibo, uint32_t index,
                                  const StorageBuffer& instanceData, uint32_t size, uint32_t count) {
      auto command = device.commandBuffer();
      {
        auto enc = command.startEncoding(device);
        enc.setFramebuffer({{diffuse,Vec4(),Preserve},{normal,Vec4(),Preserve},{orm,Vec4(),Preserve},{motion,Vec4(),Preserve}});
        enc.setBinding(0,scene);
        enc.setBinding(1,mesh && &pso==&bough ? meshPayload : payload);
        enc.setBinding(2,instanceData);
        enc.setBinding(3,buckets);
        enc.setBinding(4,ibo);
        enc.setBinding(5,vbo);
        enc.setBinding(6,texture);
        enc.setBinding(7,Sampler::nearest(ClampMode::ClampToEdge));
        enc.setBinding(15,detail);
        enc.setBinding(16,texture);
        enc.setBinding(21,instances);
        for(auto slot : {23u,24u,25u})
          enc.setBinding(slot,texture);
        enc.setPipeline(pso);
        for(uint32_t i=0; i<count; ++i) {
          const uint32_t push[] = {index+i*size,size};
          enc.setPushData(push);
          if(mesh)
            enc.dispatchMesh(1);
          else
            enc.draw(nullptr,0,indicesPerCard);
          }
        }
      device.submit(command).wait();
      return device.readPixels(diffuse);
      };
    const auto render = [&](const RenderPipeline& pso, const StorageBuffer& ibo, uint32_t index,
                            const StorageBuffer& instanceData) {
      return renderGroups(pso,ibo,index,instanceData,1,1);
      };
    const auto full = render(bough,compactIbo,0,instances);
    const auto rgba = static_cast<const uint8_t*>(full.data());
    size_t sample = (side/2)*side;
    while(sample<(side/2+1)*side && rgba[sample*4]==0)
      ++sample;
    require(sample<(side/2+1)*side,"no covered pixel for the canopy normal test");
    const auto readNormal = [&] {
      const auto pixels = device.readPixels(normal);
      return static_cast<const uint32_t*>(pixels.data())[sample];
      };
    const auto closeNormal = readNormal();
    const auto parent = render(bough,compactIbo,1,instances);
    const auto child = render(bough,compactIbo,2,instances);
    const auto baseline = render(legacy,packedIbo,0,instances);
    require(std::memcmp(full.data(),baseline.data(),full.dataSize())==0,"steady bough differs from legacy card");
    require(readNormal()==closeNormal,"near compact and GLB crowns disagree on the detail normal");
    const auto coarseBough = render(bough,compactIbo,0,coarseInstances);
    const auto coarseNormal = readNormal();
    const auto coarseGlb = render(legacy,packedIbo,0,coarseInstances);
    require(coarseNormal!=closeNormal && readNormal()==coarseNormal,
            "coarse compact and GLB crowns must both use the vertex normal");
    require(std::memcmp(coarseBough.data(),full.data(),full.dataSize())==0 &&
            std::memcmp(coarseGlb.data(),baseline.data(),baseline.dataSize())==0,
            "canopy normal LOD changed G-buffer coverage or albedo");
    const auto jointCrown = render(bough,compactIbo,0,jointInstances);
    const auto jointWood = render(jointGeometry,packedIbo,0,jointInstances);
    require(std::memcmp(jointCrown.data(),jointWood.data(),jointCrown.dataSize())==0,
            "compact and regular geometry disagree on impostor mask");
    const auto coarseJoint = render(bough,compactIbo,0,coarseJointInstances);
    const auto coarseJointGlb = render(jointGeometry,packedIbo,0,coarseJointInstances);
    require(std::memcmp(coarseJoint.data(),jointCrown.data(),jointCrown.dataSize())==0 &&
            std::memcmp(coarseJointGlb.data(),jointWood.data(),jointWood.dataSize())==0,
            "the canopy normal LOD tag must preserve a fractional impostor joint");
    const auto covered = [](const Pixmap& pixmap, size_t i) { return static_cast<const uint8_t*>(pixmap.data())[i*4]!=0; };
    std::array<size_t,2> draws{};
    size_t jointPixels = 0;
    for(size_t i=0; i<side*side; ++i) {
      const bool p = covered(parent,i), c = covered(child,i);
      require(uint32_t(p)+uint32_t(c)==uint32_t(covered(full,i)),"bough raster masks overlap or leave a hole");
      const bool joint = covered(jointCrown,i);
      require(!joint || covered(full,i),"impostor mask introduces extra coverage");
      jointPixels += size_t(joint);
      draws[0] += size_t(p);
      draws[1] += size_t(c);
      }
    require(draws[0]>0 && draws[1]>0 && draws[0]+draws[1]<side*side,"bough raster fixture missed a side or the background");
    require(jointPixels>0 && jointPixels<draws[0]+draws[1],"impostor mask has no fractional 3D coverage");
    if(mesh) {
      // One workgroup of four nodes covers what the same nodes cover one by one, with their own bands.
      const auto single = renderGroups(bough,compactIbo,3,instances,1,4);
      const auto packedGroup = renderGroups(bough,compactIbo,3,instances,4,1);
      size_t groupPixels = 0;
      for(size_t i=0; i<side*side; ++i)
        groupPixels += size_t(covered(packedGroup,i));
      require(std::memcmp(single.data(),packedGroup.data(),single.dataSize())==0,
              "a packed bough workgroup differs from its nodes drawn one by one");
      for(uint32_t node=3; node<6; ++node) {
        const auto alone = render(bough,compactIbo,node,instances);
        size_t pixels = 0;
        for(size_t i=0; i<side*side; ++i)
          pixels += size_t(covered(alone,i));
        require(pixels>0 && pixels<groupPixels, "bough packing fixture missed a node");
        }
      std::cout << "tree bough mesh packing: " << groupPixels << " pixels of four nodes in one workgroup\n";
      // The lanes above 32: eight cards with a node of 32 vertices, then the two nodes of 32 of one command.
      for(const auto [first,size] : {std::pair(wideCards,9u), std::pair(wideNode,2u)}) {
        uint32_t groupVertices = 0, groupTriangles = 0;
        for(uint32_t i=first; i<first+size; ++i) {
          groupVertices += meshTasks[i][1] >> TreeBough::FirstIndexBits;
          groupTriangles += (meshTasks[i][3] >> 24)/3u;
          }
        require(groupVertices==uint32_t(PackedMesh::MaxVert) && groupTriangles==uint32_t(PackedMesh::MaxPrim)/2u,
                "bough packing fixture does not fill the vertices of a workgroup");
        const auto apart = renderGroups(bough,compactIbo,first,instances,1,size);
        const auto together = renderGroups(bough,compactIbo,first,instances,size,1);
        require(std::memcmp(apart.data(),together.data(),apart.dataSize())==0,
                "a bough workgroup above 32 vertices differs from its nodes drawn one by one");
        const auto last = render(bough,compactIbo,first+size-1u,instances);
        size_t pixels = 0, upper = 0;
        for(size_t i=0; i<side*side; ++i) {
          pixels += size_t(covered(together,i));
          upper += size_t(covered(last,i));
          }
        require(upper>0 && upper<pixels, "bough packing fixture missed the node in the lanes above 32");
        std::cout << "tree bough mesh packing: " << pixels << " pixels of " << size << " nodes and "
                  << groupVertices << " vertices in one workgroup\n";
        }
      }
    treeLevelBandRaster(device, render, covered, jointGeometry, packedIbo, instance, jointInstance, baseline, jointWood, mesh);
    std::cout << "tree bough " << (mesh ? "mesh" : "vertex") << " coverage: " << draws[0] << " parent and " << draws[1]
              << " child pixels, " << jointPixels << " 3D pixels in the impostor joint\n";
    }
  }

void treeBoughCutCases() {
  using namespace TreeBough;
  using namespace Tempest;
  require(!MeshLevels::canopyUsesVertexNormal(1, 180.f, 0.f) && MeshLevels::canopyUsesVertexNormal(2, 220.f, 0.f) &&
          !MeshLevels::canopyUsesVertexNormal(2, 200.f, 200.f) && MeshLevels::canopyUsesVertexNormal(1, 199.f, 200.f),
          "crown normal-map limit must follow the selected GLB or the compact cut's authored L2 height");
  const auto node = [](Vec3 lo, Vec3 hi, float px, uint32_t parent, uint32_t end) {
    const auto centre = (lo + hi) * 0.5f;
    const auto diameter = (hi - lo).length();
    return Node{{centre.x, centre.y, centre.z, diameter * 0.5f},
                lo, diameter, hi, px, px * 0.9f, px * 1.1f, parent, end};
    };
  Asset asset;
  asset.roots = {0, 5};
  asset.nodes = {
    node({-180, -180, -150}, {180, 180, 150}, 55, NoNode, 5),
    node({-180, -125,  -95}, { 10,  125,  80}, 92, 0, 4),
    node({-180, -125,  -95}, {-95,    5,  30},  0, 1, 3),
    node({ -95,   -5,  -10}, { 10,  125,  80},  0, 1, 4),
    node({  10,  -80, -150}, {180,   80, 150},  0, 0, 5),
    node({ 190, -130,  -70}, {430,  130,  80}, 64, NoNode, 8),
    node({ 190, -130,  -70}, {320,   10,  20},  0, 5, 7),
    node({ 310,    0,  -10}, {430,  130,  80},  0, 5, 8)
    };
  for(uint32_t i = 0; i < asset.nodes.size(); ++i)
    asset.ranges.push_back({i * 6, 6, 1, 1});

  std::vector<float> heights;
  for(float height = 800.f; height > 30.f; height *= 0.99f)
    heights.push_back(height);
  heights.push_back(30.f);
  const size_t bottom = heights.size() - 1;
  for(size_t i = bottom; i > 0; --i)
    heights.push_back(heights[i - 1]);

  struct alignas(16) View {
    Projection projection;
    };
  static_assert(sizeof(View) == 32);
  constexpr float treeHeight = 1600.f, focal = 810.f;
  constexpr float impostorPx = 49.f;
  require(impostorJointWeight(impostorPx, 56.f)==0.f && impostorJointWeight(impostorPx, 46.f)==1.f &&
          std::abs(impostorJointWeight(impostorPx, 51.f) - 0.5f)<1e-6f &&
          impostorJointFade(0.f)==0u && impostorJointFade(1.f)==0u,
          "impostor joint endpoints, midpoint or steady fade encoding differ");
  require(impostorJointWeight(impostorPx, 55.99828f)<=
          impostorJointWeight(impostorPx, std::nextafter(55.99828f, 0.f)),
          "impostor joint reversed between adjacent floats near the upper edge");
  std::vector<View> views;
  std::vector<uint32_t> jointWeights;
  for(const auto height : heights) {
    views.push_back({{{0.30f, 0.f, 0.9539392f}, treeHeight * focal / height, focal, 15.f}});
    jointWeights.push_back(impostorJointFade(impostorJointWeight(impostorPx, height)));
    }

#ifdef __APPLE__
  MetalApi api;
#else
  VulkanApi api;
#endif
  Device device(api);
  const auto shader = GothicShader::get("tree_bough_cut_test.comp.sprv");
  const auto pipeline = device.pipeline(device.shader(shader.data, shader.len));
  require(pipeline.sizeofBuffer(0, 2) == 2 * sizeof(Node), "tree bough node layout on GPU");
  require(pipeline.sizeofBuffer(1, 2) == 2 * sizeof(View), "tree bough projection layout on GPU");

  const size_t stride = asset.nodes.size() + 1;
  std::vector<std::array<uint32_t, 4>> cuts(stride * views.size());
  const auto nodes = device.ssbo(asset.nodes);
  const auto projections = device.ssbo(views);
  const auto joints = device.ssbo(jointWeights);
  auto output = device.ssbo(Uninitialized, cuts.size() * sizeof(cuts.front()));
  auto command = device.commandBuffer();
  {
    auto encoder = command.startEncoding(device);
    encoder.setPipeline(pipeline);
    encoder.setBinding(0, nodes);
    encoder.setBinding(1, projections);
    encoder.setBinding(3, output);
    encoder.setBinding(4, joints);
    encoder.dispatchThreads(1);
  }
  device.submit(command).wait();
  device.readBytes(output, cuts.data(), cuts.size() * sizeof(cuts.front()));

  uint32_t binaryHistory = 0;
  std::vector<Selected> selected;
  std::vector<float> previousWeights(asset.nodes.size());
  uint32_t sawBands = 0;
  float previousJointWeight = 0.f;
  uint32_t activeJointSamples = 0;
  constexpr uint32_t weightMask = 0xffffffu, fadeActive = 0x2000000u;
  for(size_t sample = 0; sample < heights.size(); ++sample) {
    const auto& view = views[sample].projection;
    const auto gpuCut = std::span(cuts).subspan(sample * stride, stride);
    const auto label = "tree bough cut at " + std::to_string(heights[sample]) + " px";
    select(asset, view, selected);
    require(gpuCut[0][0] == selected.size(), label + " count differs on GPU");
    require(gpuCut[0][2] == 0, label + " two fade sides disagree on complementary coverage");
    const float jointWeight = impostorJointWeight(impostorPx, heights[sample]);
    require(sample <= bottom ? jointWeight >= previousJointWeight : jointWeight <= previousJointWeight,
            label + " impostor weight is not monotone with height");
    previousJointWeight = jointWeight;
    const uint32_t gpuJoint = gpuCut[0][3];
    if(jointWeight>0.f && jointWeight<1.f) {
      const int cpuRank = int(std::round(jointWeight * float(weightMask)));
      require((gpuJoint & ~weightMask)==fadeActive && std::abs(int(gpuJoint & weightMask)-cpuRank)<=1,
              label + " GPU impostor mask rank differs from CPU weight");
      ++activeJointSamples;
      } else {
      require(gpuJoint==0u, label + " impostor mask active outside the joint");
      }
    if(sample == 0 || sample == bottom || sample + 1 == heights.size())
      require(selected.size() == (sample == bottom ? 2u : 5u), label + " did not reach the endpoint cut");
    if(sample > bottom) {
      const auto outbound = std::span(cuts).subspan((heights.size() - 1 - sample) * stride, stride);
      require(std::ranges::equal(gpuCut.subspan(1, selected.size()), outbound.subspan(1, outbound[0][0])),
              label + " cut differs by direction");
      require(gpuJoint==outbound[0][3], label + " impostor mask differs by direction");
      }
    std::vector<const Selected*> byNode(asset.nodes.size());
    bool needCompact = false;
    for(size_t i = 0; i < selected.size(); ++i) {
      const auto& entry = selected[i];
      const auto& gpu = gpuCut[1 + i];
      require(gpu[0] == entry.node && gpu[1] == entry.transition &&
              std::abs(std::bit_cast<float>(gpu[2]) - entry.weight) < 0.0001f,
              label + " node or transition differs on GPU");
      require(byNode[entry.node] == nullptr, label + " selected a node twice");
      byNode[entry.node] = &entry;
      needCompact |= entry.transition != 0 || asset.nodes[entry.node].subtreeEnd != entry.node + 1;
      }
    require(gpuCut[0][1] == uint32_t(needCompact), label + " ordinary meshlet path differs");

    uint32_t binaryCut = 0;
    for(uint32_t i = 0; i < asset.nodes.size();) {
      const auto& n = asset.nodes[i];
      const uint32_t bit = 1u << i;
      const float limit = n.thresholdPx * ((binaryHistory & bit) != 0 ? MeshLevels::Hysteresis : 1.f);
      const bool coarse = n.highPx > 0.f && screenSize(n, view) < limit;
      binaryHistory = (binaryHistory & ~bit) | (coarse ? bit : 0u);
      if(coarse || n.subtreeEnd == i + 1)
        binaryCut |= bit;
      i = coarse ? n.subtreeEnd : i + 1;
      }
    for(uint32_t i = 0; i < asset.nodes.size(); ++i) {
      const auto& n = asset.nodes[i];
      const float weight = parentWeight(n, screenSize(n, view));
      require(sample <= bottom ? weight >= previousWeights[i] : weight <= previousWeights[i],
              label + " weight is not monotone with height");
      previousWeights[i] = weight;
      }
    for(uint32_t leaf = 0; leaf < asset.nodes.size(); ++leaf) {
      if(asset.nodes[leaf].subtreeEnd != leaf + 1)
        continue;
      uint32_t owner = NoNode, coverage = 0;
      for(uint32_t i = leaf; i != NoNode; i = asset.nodes[i].parent) {
        const float weight = previousWeights[i];
        if(weight > 0.f && weight < 1.f) {
          require(owner == NoNode, label + " overlapping bands");
          owner = i;
          sawBands |= 1u << i;
          }
        coverage += byNode[i] != nullptr;
        }
      require(coverage == (owner == NoNode ? 1u : 2u), label + " missing or extra versions for a leaf");
      for(uint32_t i = leaf; i != NoNode; i = asset.nodes[i].parent) {
        const auto* entry = byNode[i];
        if(owner == NoNode) {
          require((entry != nullptr) == ((binaryCut >> i) & 1u) && (entry == nullptr || entry->transition == 0u),
                  label + " differs from the binary cut outside bands");
          } else if(entry != nullptr) {
          const uint32_t transition = (owner + 1u) | (i == owner ? 0u : DescendantSide);
          require(entry->transition == transition && entry->weight == previousWeights[owner],
                  label + " versions do not share the active band's owner and weight");
          }
        }
      }
    }
  require(sawBands == ((1u << 0) | (1u << 1) | (1u << 5)), "tree bough fixture missed a band");
  std::cout << "tree bough cut: " << heights.size() << " CPU/GPU samples from 800 to 30 px and back\n";
  require(activeJointSamples>2, "tree bough fixture missed the impostor joint");
  std::cout << "tree bough impostor joint: " << activeJointSamples << " fractional CPU/GPU samples\n";
  treeLevelBandPicks();
  treeLevelOrderCase();
  treeLevelBandMasks(device);
  treeBoughGroupCases(device);
  treeBoughRasterCases(device);
  }
