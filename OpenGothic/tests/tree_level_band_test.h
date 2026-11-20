// Included by tree_bough_cut_test.h. The level bands of a tree with a bough hierarchy (graphics/treelevelband.h).

#include "graphics/treelevelband.h"

// Out from 900 to 30 px and back by 1 %: the coarser GLB's share grows monotonically through each band and no two
// bands of a scale overlap; outside the bands the finer side is the hard pick it replaces, inside them that pick is one
// of the two sides. The shadow scale (MeshLevels::pickShadow) likewise
void treeLevelBandPicks() {
  constexpr float px[] = {400.f, 200.f, 100.f, 50.f};
  std::vector<float> heights;
  for(float height = 900.f; height > 30.f; height *= 0.99f)
    heights.push_back(height);
  for(size_t i = heights.size(); i > 0; --i)
    heights.push_back(heights[i - 1]);
  uint8_t hard = 0, hardShadow = 0;
  bool    shadowSeparate = false;
  TreeLevelBand::Pick previous {}, previousShadow {};
  uint32_t mainBands = 0, shadowBands = 0;
  for(size_t i = 0; i < heights.size(); ++i) {
    const float height = heights[i];
    const bool  outbound = i < heights.size()/2;
    const auto  label = "level band at " + std::to_string(height) + " px";
    const auto  band = TreeLevelBand::main(px, 4, height);
    hard = MeshLevels::pick(px, 4, hard, height);
    const auto shadow = TreeLevelBand::shadow(4, height, band.fine);
    hardShadow = MeshLevels::pickShadow(4, hardShadow, shadowSeparate, height, hard, true, MeshLevels::ShadowNearDefaultPx);
    shadowSeparate = hardShadow!=hard;
    for(const auto& [now, before, step, name] : {std::tuple(band, previous, hard, "main"),
                                                  std::tuple(shadow, previousShadow, hardShadow, "shadow")}) {
      require(now.coarse==now.fine || now.coarse==now.fine+1 || (now.fine==0 && now.coarse==3),
              label + " " + name + " band joins levels that are not adjacent on its scale");
      const float share = now.active() ? now.weight : 0.f;
      if(i > 0 && now.fine==before.fine && now.coarse==before.coarse && now.active())
        require(outbound ? share >= before.weight : share <= before.weight, label + " " + name + " share is not monotone");
      if(i > 0 && !now.active() && !before.active())
        require(outbound ? now.fine >= before.fine : now.fine <= before.fine, label + " " + name + " level steps back");
      if(now.active())
        require(step==now.fine || step==now.coarse, label + " " + name + " band leaves out the hard pick");
      else
        require(step==now.fine, label + " " + name + " differs from the hard pick outside its bands");
      }
    mainBands += band.active();
    shadowBands += shadow.active();
    previous = band;
    previousShadow = shadow;
    }
  require(mainBands > 20 && shadowBands > 10, "level band fixture missed the bands");
  std::cout << "tree level bands: " << heights.size() << " heights, " << mainBands << " in a main band and "
            << shadowBands << " in a shadow band\n";
  }

// GPU: both sides of each word cover each cell once in either scale; the coarser side's share follows the word
void treeLevelBandMasks(Tempest::Device& device) {
  using namespace Tempest;
  const std::pair<float,float> shares[] = {{0.4f, 0.f}, {0.f, 0.7f}, {0.15f, 0.85f}, {0.999f, 0.001f}};
  std::vector<uint32_t> words;
  for(const auto& [main, shadow] : shares)
    words.push_back(TreeLevelBand::word({0, 1, main}, {3, 4, shadow}, false));
  require(TreeLevelBand::word({1, 1, 0.f}, {4, 4, 0.f}, false)==0u, "a level band word outside both bands");
  const auto code = GothicShader::get("tree_level_band_test.comp.sprv");
  const auto pipeline = device.pipeline(device.shader(code.data, code.len));
  const auto input = device.ssbo(words);
  auto output = device.ssbo(Uninitialized, words.size()*sizeof(std::array<uint32_t,4>));
  auto command = device.commandBuffer();
  {
    auto encoder = command.startEncoding(device);
    encoder.setPipeline(pipeline);
    encoder.setBinding(0, input);
    encoder.setBinding(1, output);
    encoder.dispatchThreads(1);
  }
  device.submit(command).wait();
  std::vector<std::array<uint32_t,4>> sides(words.size());
  device.readBytes(output, sides.data(), sides.size()*sizeof(sides.front()));
  for(size_t i = 0; i < words.size(); ++i) {
    const auto label = "level band mask " + std::to_string(shares[i].first) + "/" + std::to_string(shares[i].second);
    require(sides[i][0]==0u, label + ": a cell drawn by both sides or by none");
    const float expect[] = {shares[i].first, shares[i].second};
    for(size_t scale = 0; scale < 2; ++scale)
      require(std::abs(float(sides[i][1+scale]) - expect[scale]*4096.f) < 160.f,
              label + ": the coarser side's share differs from the word");
    }
  std::cout << "tree level band masks: " << words.size() << " words, both sides complementary over 4096 cells\n";
  }

// The wood's two sides through the masked G-buffer pipeline: with no band it draws as the legacy one; in a band the
// sides split the covered pixels, alone and inside the impostor joint's 3D side
template<class Render, class Covered>
void treeLevelBandRaster(Tempest::Device& device, const Render& render, const Covered& covered,
                         const Tempest::RenderPipeline& masked, const Tempest::StorageBuffer& ibo,
                         const float (&instance)[16], const float (&joint)[16], const Tempest::Pixmap& legacy,
                         const Tempest::Pixmap& jointWood, bool mesh) {
  const auto side = [&](const float (&base)[16], bool coarse) {
    float data[16];
    std::ranges::copy(base, data);
    data[14] = std::bit_cast<float>(TreeLevelBand::word({0, 1, 0.4f}, {}, coarse)); // the static instance's deform slot
    return render(masked, ibo, 0, device.ssbo(data, sizeof(data)));
    };
  const auto steady = render(masked, ibo, 0, device.ssbo(instance, sizeof(instance)));
  require(std::memcmp(steady.data(), legacy.data(), steady.dataSize())==0, "masked wood without a band differs");
  const auto fine = side(instance, false), coarse = side(instance, true);
  const auto jointFine = side(joint, false), jointCoarse = side(joint, true);
  size_t pixels = 0, coarsePixels = 0, jointPixels = 0;
  for(size_t i = 0; i < size_t(legacy.w())*legacy.h(); ++i) {
    require(uint32_t(covered(fine, i)) + uint32_t(covered(coarse, i))==uint32_t(covered(legacy, i)),
            "level band sides overlap or leave a hole");
    require(uint32_t(covered(jointFine, i)) + uint32_t(covered(jointCoarse, i))==uint32_t(covered(jointWood, i)),
            "level band sides differ from the impostor joint's 3D side");
    pixels += covered(legacy, i);
    coarsePixels += covered(coarse, i);
    jointPixels += covered(jointCoarse, i);
    }
  require(coarsePixels > 0 && coarsePixels < pixels && jointPixels > 0, "level band fixture missed a side");
  std::cout << "tree level band " << (mesh ? "mesh" : "vertex") << " coverage: " << coarsePixels << " of " << pixels
            << " pixels coarse, " << jointPixels << " coarse inside the joint\n";
  }
