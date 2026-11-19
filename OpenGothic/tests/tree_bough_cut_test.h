// Included by mesh_gltf_test.cpp.

void treeBoughCutCases() {
  using namespace TreeBough;
  using namespace Tempest;
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
  std::vector<View> views;
  for(const auto height : heights)
    views.push_back({{{0.30f, 0.f, 0.9539392f}, treeHeight * focal / height, focal, 15.f}});

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
  const auto roots = device.ssbo(asset.roots);
  auto output = device.ssbo(Uninitialized, cuts.size() * sizeof(cuts.front()));
  auto command = device.commandBuffer();
  {
    auto encoder = command.startEncoding(device);
    encoder.setPipeline(pipeline);
    encoder.setBinding(0, nodes);
    encoder.setBinding(1, projections);
    encoder.setBinding(2, roots);
    encoder.setBinding(3, output);
    encoder.dispatchThreads(1);
  }
  device.submit(command).wait();
  device.readBytes(output, cuts.data(), cuts.size() * sizeof(cuts.front()));

  uint32_t history = 0;
  std::vector<Selected> selected;
  bool sawParentBand = false, sawChildBand = false, sawHysteresis = false;
  for(size_t sample = 0; sample < heights.size(); ++sample) {
    const auto& view = views[sample].projection;
    const auto gpuCut = std::span(cuts).subspan(sample * stride, stride);
    const auto label = "tree bough cut at " + std::to_string(heights[sample]) + " px";
    select(asset, view, std::span(&history, 1), selected);
    require(gpuCut[0][0] == selected.size(), label + " count differs on GPU");
    require(gpuCut[0][1] == history, label + " hysteresis state differs on GPU");
    if(sample == 0 || sample == bottom || sample + 1 == heights.size())
      require(selected.size() == (sample == bottom ? 2u : 5u), label + " did not reach the endpoint cut");
    if(sample > bottom)
      sawHysteresis |= gpuCut[0][0] != cuts[(heights.size() - 1 - sample) * stride][0];
    uint32_t marked = 0;
    bool needCompact = false;
    for(size_t i = 0; i < selected.size(); ++i) {
      const auto& entry = selected[i];
      const auto& gpu = gpuCut[1 + i];
      require(gpu[0] == entry.node && gpu[1] == entry.transition &&
              std::abs(std::bit_cast<float>(gpu[2]) - entry.weight) < 0.0001f,
              label + " node or transition differs on GPU");
      const uint32_t bit = 1u << entry.node;
      require((marked & bit) == 0, label + " selected a node twice");
      marked |= bit;
      needCompact |= entry.transition != 0 || asset.nodes[entry.node].subtreeEnd != entry.node + 1;
      sawParentBand |= entry.transition != 0 && (entry.transition & DescendantSide) == 0;
      sawChildBand |= (entry.transition & DescendantSide) != 0;
      }
    require(gpuCut[0][2] == uint32_t(needCompact) && gpuCut[0][3] == 0,
            label + " ordinary meshlet path or probe history differs");
    for(uint32_t leaf = 0; leaf < asset.nodes.size(); ++leaf) {
      if(asset.nodes[leaf].subtreeEnd != leaf + 1)
        continue;
      uint32_t coverage = 0, bands = 0;
      for(uint32_t i = leaf; i != NoNode; i = asset.nodes[i].parent) {
        const auto& n = asset.nodes[i];
        const float weight = parentWeight(n, screenSize(n, view));
        coverage += (marked >> i) & 1u;
        bands += weight > 0.f && weight < 1.f ? 1u : 0u;
        }
      require(coverage == 1 && bands <= 1, label + " did not cover a leaf exactly once or overlapped bands");
      }
    }
  require(sawParentBand && sawChildBand && sawHysteresis, "tree bough fixture missed a band edge or hysteresis");
  std::cout << "tree bough cut: " << heights.size() << " CPU/GPU samples from 800 to 30 px and back\n";
  }
