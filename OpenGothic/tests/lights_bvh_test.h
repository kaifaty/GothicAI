#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`.
// The lamps' tree of bounding boxes that the GI probes walk to find the lamps at a hit (probe_lighting.comp,
// localLight): the engine's own lights_morton.comp, lights_topology.comp and lights_bvh.comp, run as
// Renderer::prepareLightsBvh runs them. The lamps: five of the world, the fire bow's, three fire arrows stuck within
// one cell of the Morton grid in slots freed before (ids 0, 1 and 4, which the sort leaves in the order 0, 4, 1) and
// three freed slots of range 0. Every box node is reached once from the root, every lamp once, each box holds its
// child's lamp ranges, and the bit of every box node is back at 0 after the build - two frames in a row on the same
// bits, as the game keeps them. Before, lamps of one key were told apart by their ids: the tree took a cycle through
// the arrows' nodes, and the walk of a probe near them never ended (Xid 109, CTX SWITCH TIMEOUT on the RTX 3080;
// reports/render-features/2026-11-20-itemfx-rt-hang/README.md).
void lightsBvhRecords(Tempest::Device& device) {
  Resources resources(device);
  Shaders   shaders;
  shaders.waitCompiler();
  if(!Shaders::isLightsTreeSupported()) {
    std::cout << "lights bvh: skipped, no lamps' tree on this device\n";
    return;
    }

  struct Light { float pos[3], range, color[3], cookie, sourceSize, nearLimit, padding[2]; }; // LightSource, scene.glsl
  struct Node  {                                                                            // BVHNode, lights_common.glsl
    float lmin[3]; uint32_t padd0;
    float lmax[3]; uint32_t ptrL;
    float rmin[3]; uint32_t padd1;
    float rmax[3]; uint32_t ptrR;
    };
  require(sizeof(Light)==shaders.lightsBvh.sizeofBuffer(0, 1), "the test's LightSource follows scene.glsl");
  require(sizeof(Node)==shaders.lightsBvh.sizeofBuffer(4, 1), "the test's BVHNode follows lights_common.glsl");

  const auto lamp = [](float x, float y, float z, float range, float r, float g, float b) {
    return Light{{x,y,z}, range, {r,g,b}, 0, 6, 0, {}};
    };
  const Light arrow[3] = { // a few cm apart: one key of the 1024^3 grid over these lamps' box (400 m wide)
    lamp(30020, 1010, 10020, 400, 1.0f, 0.60f, 0.2f),
    lamp(30030, 1010, 10024, 400, 1.0f, 0.55f, 0.2f),
    lamp(30024, 1012, 10030, 400, 1.0f, 0.50f, 0.2f),
    };
  const Light freed = {}; // LightGroup::free: range 0
  const std::vector<Light> lights = {
    arrow[0],
    arrow[1],
    lamp(    0,    0,     0, 500, 0.9f, 0.7f, 0.5f),
    lamp(40000, 3000,     0, 500, 0.8f, 0.7f, 0.5f),
    arrow[2],
    lamp(    0,    0, 40000, 700, 0.7f, 0.7f, 0.5f),
    lamp(40000,    0, 40000, 300, 0.6f, 0.7f, 0.5f),
    lamp(20000, 8000, 20000, 400, 0.5f, 0.7f, 0.5f),
    lamp(29900, 1080, 10100, 200, 1.0f, 0.4f, 0.1f), // the bow's own fire by the hero
    freed, freed, freed,
    };
  const uint32_t numLights = uint32_t(lights.size()), active = 9, internal = active-1;
  uint32_t numLightsPot = 1;
  while(numLightsPot<numLights)
    numLightsPot <<= 1;

  const std::vector<uint32_t> noBits((numLights+31)/32, 0);
  auto lightBuf = device.ssbo(lights);
  auto morton   = device.ssbo(Tempest::Uninitialized, shaders.lightsBvh.sizeofBuffer(1, numLightsPot));
  auto alux     = device.ssbo(Tempest::Uninitialized, shaders.lightsBvh.sizeofBuffer(2, numLights*2));
  auto ctrl     = device.ssbo(noBits);
  auto bvh      = device.ssbo(Tempest::Uninitialized, shaders.lightsBvh.sizeofBuffer(4, numLights*2));

  for(int frame=0; frame<2; ++frame) {
    auto command = device.commandBuffer();
    {
    auto enc = command.startEncoding(device);
    struct Push { uint32_t numLightsPot, numLights; } push = {numLightsPot, numLights};
    enc.setFramebuffer({});
    enc.setPushData(push);
    enc.setBinding(0, lightBuf);
    enc.setBinding(1, morton);
    enc.setBinding(2, alux);
    enc.setBinding(3, ctrl);
    enc.setPipeline(shaders.lightsMorton);
    enc.dispatch(1);
    enc.setBinding(4, bvh);
    enc.setPipeline(shaders.lightsTopology);
    enc.dispatchThreads(numLights);
    enc.setPipeline(shaders.lightsBvh);
    enc.dispatchThreads(numLights);
    }
    device.submit(command).wait();

    uint32_t header[4] = {};
    device.readBytes(morton, header, sizeof(header));
    require(header[0]==active, "lamps' tree: the lamps of range 0 are left out");

    std::vector<Node> nodes(numLights*2);
    device.readBytes(bvh, nodes.data(), nodes.size()*sizeof(Node));
    std::vector<uint32_t> bits(noBits.size());
    device.readBytes(ctrl, bits.data(), bits.size()*sizeof(uint32_t));

    // the walk of localLight, every node once: a box reached twice is a cycle or a node of two parents
    constexpr uint32_t BoxNode = 0x10000000u, LightNode = 0x40000000u, Index = 0x0FFFFFFFu;
    std::vector<bool>     boxSeen(internal, false);
    std::vector<uint32_t> lampSeen(lights.size(), 0);
    struct Box { float lo[3], hi[3]; };
    const auto holds = [](const float* lo, const float* hi, const Box& b) {
      for(int a=0; a<3; ++a)
        if(std::abs(lo[a]-b.lo[a])>0.01f || std::abs(hi[a]-b.hi[a])>0.01f)
          return false;
      return true;
      };
    const auto walk = [&](const auto& self, uint32_t ptr) -> Box {
      const uint32_t id = ptr & Index;
      if((ptr & ~Index)==LightNode) {
        require(id>=internal && id<internal+active, "lamps' tree: a lamp node past the lamps");
        const Node& n = nodes[id];
        float range = 0;
        std::memcpy(&range, &n.ptrL, sizeof(range));
        size_t found = lights.size();
        for(size_t i=0; i<lights.size(); ++i)
          if(lights[i].range>0 && std::memcmp(lights[i].pos, n.lmin, sizeof(n.lmin))==0 &&
             std::memcmp(lights[i].color, n.lmax, sizeof(n.lmax))==0 && lights[i].range==range)
            found = i;
        require(found<lights.size(), "lamps' tree: a lamp node holds no lamp of the scene");
        ++lampSeen[found];
        const Light& l = lights[found];
        return Box{{l.pos[0]-l.range, l.pos[1]-l.range, l.pos[2]-l.range},
                   {l.pos[0]+l.range, l.pos[1]+l.range, l.pos[2]+l.range}};
        }
      require((ptr & ~Index)==BoxNode && id<internal, "lamps' tree: a box pointer past the box nodes");
      require(!boxSeen[id], "lamps' tree: a box node is reached twice - a cycle or two parents, which the walk of "
                            "the probes goes round without end (lamps of one key out of the order of the sort)");
      boxSeen[id] = true;
      const Node n = nodes[id];
      const Box  l = self(self, n.ptrL), r = self(self, n.ptrR);
      require(holds(n.lmin, n.lmax, l) && holds(n.rmin, n.rmax, r),
              "lamps' tree: a box node holds the boxes of its children's lamp ranges");
      Box ret = {};
      for(int a=0; a<3; ++a) {
        ret.lo[a] = std::min(l.lo[a], r.lo[a]);
        ret.hi[a] = std::max(l.hi[a], r.hi[a]);
        }
      return ret;
      };
    walk(walk, 0 | BoxNode);
    for(size_t i=0; i<lights.size(); ++i)
      require(lampSeen[i]==(lights[i].range>0 ? 1u : 0u), "lamps' tree: every lamp of a range is in the tree once");
    require(std::all_of(boxSeen.begin(), boxSeen.end(), [](bool b) { return b; }),
            "lamps' tree: every box node is reached from the root");
    require(std::all_of(bits.begin(), bits.end(), [](uint32_t b) { return b==0; }),
            "lamps' tree: both children reach every box node, its bit is back at 0 for the next frame");
    }
  }
