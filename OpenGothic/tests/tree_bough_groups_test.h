// Included by tree_bough_cut_test.h. Whole mesh workgroups of the compact foliage cut (graphics/treeboughgroups.h,
// which mesh_gltf_test.cpp includes).

// Nodes for a mesh workgroup above 32 vertices: eight cards of four vertices in the lower half of a 4x4 grid of
// cells, a node of 32 vertices in the upper half and one more over the cards. Tasks: {first index, vertices, indices}.
std::vector<std::array<uint32_t,3>> treeBoughWideNodes(std::vector<Resources::Vertex>& vbo, std::vector<uint32_t>& ibo) {
  std::vector<std::array<uint32_t,3>> nodes;
  const auto node = [&](uint32_t firstCell, uint32_t cells) {
    const uint32_t firstVertex = uint32_t(vbo.size());
    nodes.push_back({uint32_t(ibo.size()), cells*4u, cells*6u});
    ibo.push_back((firstVertex << 16) | (cells*4u));
    for(uint32_t i=0; i<cells; ++i) {
      const uint32_t cell = firstCell+i, at = firstVertex+i*4u;
      const float x = -0.95f + 0.5f*float(cell%4u), y = -0.95f + 0.5f*float(cell/4u);
      for(const auto [dx,dy] : {std::pair(0.f,0.f),std::pair(0.4f,0.f),std::pair(0.4f,0.4f),std::pair(0.f,0.4f)})
        vbo.push_back({{x+dx,y+dy,0.4f},{0.f,0.f,1.f},{dx*2.5f,dy*2.5f},0xffffffffu});
      ibo.insert(ibo.end(),{at,at+1u,at+2u,at,at+2u,at+3u});
      }
    };
  for(uint32_t card=0; card<8; ++card)
    node(card,1);
  node(8,8);
  node(0,8);
  return nodes;
  }

void treeBoughGroupCases(Tempest::Device& device) {
  using namespace Tempest;
  // tree_bough_expand.comp, stage 2, on the layout of TreeBoughRender::uploadData: `count` cut nodes of a frame
  // stand in a command of `unit` nodes per workgroup and `maxPayload` nodes at most.
  struct Case {
    uint32_t unit, maxPayload, count;
    };
  constexpr Case cases[] = {{8,13,13}, {4,6,5}, {2,4,4}, {8,9,0}, {2,3,1}, {4,8,1}, {8,9,9}};
  struct Command { // IndirectCmd of scene.glsl
    uint32_t vertexCount, instanceCount, firstVertex, firstInstance, writeOffset;
    };
  constexpr uint32_t guard = 8;
  std::vector<Command> draws;
  uint32_t capacity = 0;
  for(const auto& c:cases) {
    draws.push_back({c.unit,c.count,1u,1u,capacity});
    capacity += TreeBough::groupSlots(c.maxPayload,c.unit);
    }
  // Stale nodes of an earlier frame fill every slot the cut has not written.
  std::vector<std::array<uint32_t,4>> slots(capacity+guard);
  for(uint32_t i=0; i<slots.size(); ++i)
    slots[i] = {0xdead0000u | i, (32u << TreeBough::FirstIndexBits) | i, 0x80010000u | i, (48u << 24) | i};

  const auto shader = GothicShader::get("tree_bough_expand.comp.sprv");
  const auto pipeline = device.pipeline(device.shader(shader.data, shader.len));
  require(pipeline.sizeofBuffer(8, 2) == 2 * sizeof(Command) && pipeline.sizeofBuffer(9, 2) == 2 * sizeof(slots.front()),
          "tree bough command or payload layout on GPU");
  const SceneGlobals::UboGlobal globals{};
  const uint32_t none[64] = {};
  const auto scene = device.ssbo(&globals, sizeof(globals));
  const auto unused = device.ssbo(none, sizeof(none));
  auto mainPayload = device.ssbo(none, sizeof(none));
  auto mainDraws = device.ssbo(none, sizeof(none));
  auto state = device.ssbo(none, sizeof(none));
  auto indirect = device.ssbo(draws);
  auto payload = device.ssbo(slots);
  auto command = device.commandBuffer();
  {
    auto encoder = command.startEncoding(device);
    const uint32_t push[] = {0u, 0u, 0u, 2u}; // token, flags, nearZ, stage
    encoder.setBinding(0, scene);
    encoder.setBinding(1, mainPayload);
    encoder.setBinding(2, unused);
    encoder.setBinding(3, unused);
    encoder.setBinding(4, mainDraws);
    encoder.setBinding(5, unused);
    encoder.setBinding(6, state);
    encoder.setBinding(7, unused);
    encoder.setBinding(8, indirect);
    encoder.setBinding(9, payload);
    encoder.setBinding(10, unused);
    encoder.setPipeline(pipeline);
    encoder.setPushData(push);
    encoder.dispatchThreads(draws.size());
  }
  device.submit(command).wait();
  std::vector<Command> groups(draws.size());
  auto padded = slots;
  device.readBytes(indirect, groups.data(), groups.size() * sizeof(Command));
  device.readBytes(payload, padded.data(), padded.size() * sizeof(padded.front()));

  uint32_t empty = 0;
  for(size_t i=0; i<draws.size(); ++i) {
    const auto& c = cases[i];
    const auto& before = draws[i];
    const auto& after = groups[i];
    const auto label = "tree bough command " + std::to_string(i);
    const uint32_t whole = (c.count+c.unit-1u)/c.unit;
    require(after.instanceCount==whole, label + " does not draw ceil(nodes / group) workgroups");
    require(after.vertexCount==before.vertexCount && after.firstVertex==1u && after.firstInstance==1u &&
            after.writeOffset==before.writeOffset, label + " lost its group size, dispatch counts or payload offset");
    require(whole*c.unit<=TreeBough::groupSlots(c.maxPayload,c.unit), label + " pads beyond its payload");
    for(uint32_t at=0; at<whole*c.unit; ++at) {
      const auto& slot = padded[before.writeOffset+at];
      if(at<c.count)
        require(slot==slots[before.writeOffset+at], label + " lost a cut node to the padding");
      else
        require(slot==std::array<uint32_t,4>{}, label + " draws a stale node in its last workgroup");
      empty += uint32_t(at>=c.count);
      }
    }
  for(uint32_t at=capacity; at<capacity+guard; ++at)
    require(padded[at]==slots[at], "tree bough padding writes past the payload");

  // The capacity of a command is whole groups and no more than one group above its nodes.
  for(uint32_t group=1; group<=8; group*=2)
    for(uint32_t nodes=0; nodes<=40; ++nodes) {
      const uint32_t padding = TreeBough::groupSlots(nodes,group)-nodes;
      require((nodes+padding)%group==0 && padding<group, "tree bough payload capacity is not whole workgroups");
      }
  // Every node the loader accepts: a group fits the vertices and primitives of one mesh workgroup, and is the
  // largest power of two up to eight that does.
  uint32_t sizes = 0;
  for(uint32_t vertices=3; vertices<=TreeBough::MaxVertices; ++vertices)
    for(uint32_t indices=3; indices<=TreeBough::MaxIndices; indices+=3) {
      const uint32_t group = TreeBough::groupSize({0,indices,0,0,vertices});
      const auto fits = [&](uint32_t n) {
        return n*vertices<=uint32_t(PackedMesh::MaxVert) && n*(indices/3u)<=uint32_t(PackedMesh::MaxPrim);
        };
      require(std::has_single_bit(group) && group<=8u && fits(group),
              "tree bough group of " + std::to_string(group) + " nodes of " + std::to_string(vertices) +
              " vertices leaves the mesh workgroup");
      require(group==8u || !fits(group*2u), "tree bough group leaves half of the mesh workgroup idle");
      sizes |= group;
      }
  require(sizes==(8u | 4u | 2u), "tree bough group sizes differ from 8, 4 and 2");
  require(TreeBough::groupSize({0,6,0,0,4})==8u && TreeBough::groupSize({0,48,0,0,32})==2u,
          "tree bough group of cards or of the largest node");
  std::cout << "tree bough groups: " << draws.size() << " commands padded with " << empty << " empty nodes\n";
  }
