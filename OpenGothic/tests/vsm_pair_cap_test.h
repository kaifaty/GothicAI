#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`.
// The meshlet-page pairs of the sun's virtual shadow map and the lamps' faces (reports/engine/2026-11-16-modern-trees/
// shadow-flicker.md): the engine's own vsm_visibility_pass.comp, vsm_cull_meshlets.comp and vsm_pack_draws.comp on
// four page entries - mip 0, mip 1, a lamp's face and mip 2 - and two clusters. An object's cluster keeps the meshlets
// whose own spheres touch an entry; a cluster of one meshlet goes whole. The bins of pairs go in whole in their order,
// lamps first, as long as they fit the cap, exactly at it too; the first one that does not and all after it are left
// out, the same ones however the GPU runs; a bin left out the frame before needs 1/8 of the cap spare.
// Before, the object went with all its meshlets into every entry, and past the cap the order of the GPU's groups chose
// which pairs dropped: the shadow over a forest flickered from frame to frame.
void vsmPairCapRecords(Tempest::Device& device) {
  Resources resources(device);
  Shaders   shaders;
  shaders.waitCompiler();
  if(!Shaders::isVsmSupported()) {
    std::cout << "vsm pair cap: skipped, no virtual shadow maps on this device\n";
    return;
    }

  // the bins of vsm_common.glsl: a lamp 5 m from the camera, then the centre ring of mips 0, 1 and 2
  constexpr uint32_t Slots = 64, Draws = 2, Old = 999, Skip = 0xFFFFFFFFu, Lamp = 1, Sun0 = 16, Sun1 = 32, Sun2 = 48;
  constexpr uint32_t ObjectMeshlet = 10, SpheresAt = 2; // the object's first meshlet, its mesh's first sphere
  struct Cluster { float sphere[4]; uint32_t bucketId_commandId, firstMeshlet; int32_t meshletCount; uint32_t instanceId; };
  struct Cmd     { uint32_t vertexCount, instanceCount, firstVertex, firstInstance, writeOffset; };
  struct Light   { float pos[3], range, color[3], cookie, sourceSize, nearLimit, padding[2]; }; // LightSource, scene.glsl
  struct Header  { // VsmHeader of vsm_common.glsl
    uint32_t pageCount, meshletCount, counterM, counterV, pagePerMip[16];
    int32_t  pageBbox[16][4];
    uint32_t pageOmniCount, pageListCount, pagePlaced, omniTileOverflow, omniTileLightsMax, pairSpill;
    uint32_t candidateCount, clusterPairs, admitBits[9], admitPrevBits[9], pairDemand[16+16*16];
    };
  const auto prefix = [](uint32_t bins) { // the first bins drawn, the rest left out
    std::array<uint32_t,9> ret = {};
    for(uint32_t b=0; b<bins; ++b)
      ret[b/32] |= 1u << (b%32);
    return ret;
    };
  const size_t headerSize = shaders.vsmClear.sizeofBuffer(0, 0);
  require(headerSize>=sizeof(Header) && headerSize<sizeof(Header)+16, "the test's VsmHeader follows vsm_common.glsl");
  require(sizeof(Cluster)*2==shaders.vsmCullMeshlets.sizeofBuffer(5, 2), "the test's Cluster follows scene.glsl");
  require(sizeof(Light)==shaders.vsmCullMeshlets.sizeofBuffer(6, 1), "the test's LightSource follows scene.glsl");
  require(shaders.vsmCullMeshlets.sizeofBuffer(3, 1)==144, "Bucket of scene.glsl is DrawBuckets::BucketGpu's 144 bytes");

  // the sun's view scales x and y by 1/1024 per cm, as the game's does: vsm_cull.glsl takes a sphere's centre through the
  // view and its radius by that scale (smWidthInv). Mip 0's page (16,16) is then x and y of 0..64, mip 1's of 0..128,
  // mip 2's of 0..256; a lamp hangs 5 m over them and its face 5 looks down on them
  std::vector<uint8_t> scene(sizeof(SceneGlobals::UboGlobal), 0);
  auto sunView = Tempest::Matrix4x4::mkIdentity();
  sunView.scale(1.f/1024.f, 1.f/1024.f, 1.f);
  std::memcpy(scene.data(), &sunView, sizeof(sunView));
  const auto sunEntry = [](uint32_t mip) { // packVsmPageInfo(ivec3(16,16,mip), ivec2(1))
    return (16u << 1) | (16u << 9) | (mip << 17) | (1u << 24) | (1u << 28);
    };
  const uint32_t entries[4] = {sunEntry(0), sunEntry(1), 0x1u | (5u << 1) | (1u << 24) | (1u << 28), sunEntry(2)};
  const Light light = {{32.f, 32.f, 500.f}, 1000.f, {1.f, 1.f, 1.f}, 0.f, 0.f, 0.f, {0.f, 0.f}};

  // cluster 0: an object of six meshlets at the instance's origin (the identity). Its meshlets' spheres: 0, 1 and 5 touch
  // all sun entries, 2 those of mips 1 and 2, 3 and 4 none, and all but 3 and 4 the lamp's face. Cluster 1: one
  // meshlet of the landscape, whole in each entry but mip 2's, where it is too small (the visibility pass's 32 pixels)
  const float meshlets[6][4] = {{32,32,0,4}, {40,20,0,4}, {100,32,0,4}, {-3000,32,0,4}, {32,-3000,0,4}, {60,60,0,4}};
  std::vector<float> spheres(32*4, 0.f);
  for(size_t i=0; i<spheres.size(); i+=4)
    spheres[i+3] = 1.e6f; // any sphere read past the mesh's own would take every meshlet
  for(uint32_t i=0; i<6; ++i)
    std::memcpy(&spheres[(SpheresAt+ObjectMeshlet+i)*4], meshlets[i], sizeof(meshlets[i]));
  const Cluster clusters[2] = {
    {{0.f, 0.f, 0.f, 400.f}, (0u << 16) | 0u, ObjectMeshlet, 6, 0u},
    {{32.f, 32.f, 0.f, 20.f}, (1u << 16) | 1u, 77, 1, 0xFFFFFFFFu},
    };
  std::vector<uint32_t> bucket(2*36, 0); // Bucket of scene.glsl, std140: bboxRadius at 10, meshletSpheres at 32
  const float radius = 400.f;
  std::memcpy(&bucket[10], &radius, sizeof(radius));
  bucket[32]    = SpheresAt;
  bucket[36+32] = 0xFFFFFFFFu;
  std::vector<float> instance(16, 0.f);
  instance[0] = instance[4] = instance[8] = 1.f; // mat4x3 by columns, then fatness, animPtr, deformPtr, deformCount
  std::vector<uint32_t> older(size_t(Slots)*4); // the slots as an older frame left them: draw 0, meshlet 999
  for(uint32_t i=0; i<Slots; ++i) {
    older[i*4+0] = 0;
    older[i*4+1] = Old;
    older[i*4+2] = 0;
    older[i*4+3] = 0;
    }

  auto sceneUbo    = device.ssbo(scene.data(), scene.size());
  auto clusterBuf  = device.ssbo(clusters, sizeof(clusters));
  auto bucketBuf   = device.ssbo(bucket.data(), bucket.size()*sizeof(uint32_t));
  auto instanceBuf = device.ssbo(instance.data(), instance.size()*sizeof(float));
  auto sphereBuf   = device.ssbo(spheres.data(), spheres.size()*sizeof(float));
  auto lightBuf    = device.ssbo(&light, sizeof(light));
  auto candidates  = device.ssbo(Tempest::Uninitialized, size_t(Slots)*4*sizeof(uint32_t));
  auto payloadDst  = device.ssbo(Tempest::Uninitialized, size_t(Slots)*4*sizeof(uint32_t));
  auto indirect    = device.ssbo(Tempest::Uninitialized, 16);
  auto clearPages  = device.ssbo(Tempest::Uninitialized, shaders.vsmClear.sizeofBuffer(0, 16));
  auto pageHiZ     = device.image3d(Tempest::TextureFormat::R32U, 32, 32, 16);
  auto scratch     = device.image3d(Tempest::TextureFormat::R32U, 32, 32, 16);
  auto dbg         = device.image2d(Tempest::TextureFormat::R32U, 1, 1);

  struct Frame {
    Header                header = {};
    std::vector<Cmd>      draws;
    std::vector<uint32_t> packed;
    };
  // the packing alone, from slots and counts set here; or a whole frame: the visibility pass, the meshlets' pass and
  // the packing, under cap pairs, with the classes drawn the frame before
  const auto run = [&](bool packOnly, uint32_t cap, const std::array<uint32_t,9>& admitPrev, uint32_t written,
                       const std::vector<Cmd>& counts, const std::vector<uint32_t>& slots) {
    std::vector<uint8_t> pages(shaders.vsmClear.sizeofBuffer(0, 16), 0);
    Header header = {};
    header.pageCount    = 4;
    header.meshletCount = written;
    std::copy(admitPrev.begin(), admitPrev.end(), header.admitPrevBits);
    std::memcpy(pages.data(), &header, sizeof(header));
    std::memcpy(pages.data()+headerSize, entries, sizeof(entries));
    auto pageBuf    = device.ssbo(pages.data(), pages.size());
    auto cmdBuf     = device.ssbo(counts.data(), counts.size()*sizeof(Cmd));
    auto payloadSrc = device.ssbo(slots.data(), slots.size()*sizeof(uint32_t));

    auto command = device.commandBuffer();
    {
    auto enc = command.startEncoding(device);
    if(!packOnly) {
      // vsm_clear.comp zeroes its page table: here the receivers' depth of every page, so no cluster is behind them
      enc.setBinding(0, clearPages);
      enc.setBinding(1, pageHiZ);
      enc.setBinding(2, scratch);
      enc.setPipeline(shaders.vsmClear);
      enc.dispatchThreads(32, 32, 16);

      struct Push { uint32_t meshletCount, candidateCap; } push = {2, cap};
      enc.setBinding(0, sceneUbo);
      enc.setBinding(1, candidates);
      enc.setBinding(2, instanceBuf);
      enc.setBinding(3, bucketBuf);
      enc.setBinding(5, clusterBuf);
      enc.setBinding(6, lightBuf);
      enc.setBinding(7, pageHiZ);
      enc.setBinding(8, pageBuf);
      enc.setBinding(9, dbg);
      enc.setPushData(&push, sizeof(push));
      enc.setPipeline(shaders.vsmVisibilityPass);
      enc.dispatchThreads(2, 17);

      struct CullPush { uint32_t pairCap, emit; } cull = {cap, 0};
      enc.setBinding(4, cmdBuf);
      enc.setBinding(7, sphereBuf);
      enc.setBinding(9, payloadSrc);
      enc.setPipeline(shaders.vsmCullMeshlets);
      for(cull.emit=0; cull.emit<2; ++cull.emit) {
        enc.setPushData(&cull, sizeof(cull));
        enc.dispatch(3);
        }
      }

    enc.setBinding(0, sceneUbo);
    enc.setBinding(1, payloadSrc);
    enc.setBinding(2, payloadDst);
    enc.setBinding(3, cmdBuf);
    enc.setBinding(4, pageBuf);
    enc.setBinding(5, indirect);
    enc.setPipeline(shaders.vsmPackDraw0);
    enc.dispatch(1);

    enc.setPushData(&cap, sizeof(cap));
    enc.setPipeline(shaders.vsmPackDraw1);
    enc.dispatch((cap+127)/128);
    }
    device.submit(command).wait();

    Frame f;
    device.readBytes(pageBuf, &f.header, sizeof(f.header));
    f.draws.resize(Draws);
    device.readBytes(cmdBuf, f.draws.data(), f.draws.size()*sizeof(Cmd));
    f.packed.resize(size_t(Slots)*4);
    device.readBytes(payloadDst, f.packed.data(), f.packed.size()*sizeof(uint32_t));
    return f;
    };
  const std::vector<Cmd> zero(Draws, Cmd{64*3, 0, 1, 1, 0});
  // the pairs of each draw by entry: meshlet ids of the object (draw 0), counts of the landscape's (draw 1)
  const auto pairsOf = [&](const Frame& f, uint32_t draw, uint32_t entry) {
    std::vector<uint32_t> ret;
    require(draw+1==Draws || f.draws[draw].writeOffset+f.draws[draw].instanceCount==f.draws[draw+1].writeOffset,
            "a draw ends where the next one's range starts");
    for(uint32_t i=0; i<f.draws[draw].instanceCount; ++i) {
      const uint32_t* p = &f.packed[size_t(f.draws[draw].writeOffset+i)*4];
      require(p[1]!=Old && p[2]==draw && (draw==0 ? p[0]==0 : p[0]==0xFFFFFFFFu && p[1]==77),
              "a draw takes only its own cluster's meshlets, none an older frame left in the slots");
      if(p[3]==entry)
        ret.push_back(p[1]);
      }
    std::sort(ret.begin(), ret.end());
    return ret;
    };
  const auto ids = [&](std::initializer_list<uint32_t> l) {
    std::vector<uint32_t> ret;
    for(auto i:l)
      ret.push_back(ObjectMeshlet+i);
    return ret;
    };

  // the packing skips a slot the meshlets' pass reserved and left (VSM_PAIR_SKIP): draw 0 reserved two, found one
  {
  std::vector<uint32_t> slots = older;
  const uint32_t first[3][4] = {{0, ObjectMeshlet, 0, 0}, {Skip, Skip, Skip, Skip}, {0xFFFFFFFFu, 77, (1u << 16) | 1u, 0}};
  std::memcpy(slots.data(), first, sizeof(first));
  std::vector<Cmd> counts = zero;
  counts[0].instanceCount = 2;
  counts[1].instanceCount = 1;
  const Frame f = run(true, Slots, {}, 3, counts, slots);
  require(f.header.pairSpill==0 && f.draws[0].instanceCount==1 && f.draws[1].writeOffset==2 &&
          f.draws[1].instanceCount==1 && f.packed[1]==ObjectMeshlet && f.packed[2*4+1]==77,
          "the packing skips a slot left with VSM_PAIR_SKIP, and no draw takes it");
  }

  // room for all: the object's meshlets by their spheres in each entry, the landscape's meshlet whole
  const auto admitted = [](const Frame& f) {
    std::array<uint32_t,9> ret = {};
    std::copy(std::begin(f.header.admitBits), std::end(f.header.admitBits), ret.begin());
    return ret;
    };
  const Frame all = run(false, Slots, {}, 0, zero, older);
  const auto demand = [](const Frame& f) {
    return " (lamp " + std::to_string(f.header.pairDemand[Lamp]) + ", mip 0 " + std::to_string(f.header.pairDemand[Sun0]) +
           ", mip 1 " + std::to_string(f.header.pairDemand[Sun1]) + ", mip 2 " + std::to_string(f.header.pairDemand[Sun2]) +
           ", candidates " + std::to_string(f.header.candidateCount) + ", drawn " + std::to_string(f.header.meshletCount) + ")";
    };
  require(all.header.pairDemand[Sun0]==4 && all.header.pairDemand[Sun1]==5 && all.header.pairDemand[Lamp]==5 &&
          all.header.pairDemand[Sun2]==4,
          ("the demand of each bin: the meshlets whose spheres touch its entries"+demand(all)).c_str());
  require(all.header.clusterPairs==4*6+3*1,
          ("whole clusters would ask a pair for each meshlet in each entry: "+std::to_string(all.header.clusterPairs)).c_str());
  require(admitted(all)==prefix(16+16*16) && all.header.meshletCount==18 && all.header.pairSpill==0,
          ("with room for all every bin is drawn"+demand(all)).c_str());
  require(pairsOf(all, 0, 0)==ids({0, 1, 5}) && pairsOf(all, 0, 1)==ids({0, 1, 2, 5}) &&
          pairsOf(all, 0, 2)==ids({0, 1, 2, 5}) && pairsOf(all, 0, 3)==ids({0, 1, 2, 5}),
          "an object's entry takes the meshlets whose spheres touch it");
  require(pairsOf(all, 1, 0).size()==1 && pairsOf(all, 1, 1).size()==1 && pairsOf(all, 1, 2).size()==1 &&
          pairsOf(all, 1, 3).empty(), "a cluster without spheres goes whole into each entry");

  // a cap of 13: the lamp's 5 and mip 0's 4 go in, mip 1's 5 would make 14: it and all after it are left out, mip 2's 4
  // too, though they would fit
  const Frame edge = run(false, 13, admitted(all), 0, zero, older);
  require(admitted(edge)==prefix(Sun1) && edge.header.meshletCount==9 && edge.header.pairSpill==0,
          ("bins go in whole in their order, lamps first; the first past the cap and all after it are left out"+
           demand(edge)).c_str());
  require(pairsOf(edge, 0, 0)==ids({0, 1, 5}) && pairsOf(edge, 0, 1).empty() && pairsOf(edge, 0, 2)==ids({0, 1, 2, 5}) &&
          pairsOf(edge, 0, 3).empty() && pairsOf(edge, 1, 1).empty(),
          "a bin left out has no pair, the bins drawn all theirs");

  // a cap of 14 holds the lamp, mip 0 and mip 1 (14), but mip 1 was left out the frame before and needs 14 under 7/8
  // of the cap: it stays out
  const Frame held = run(false, 14, admitted(edge), 0, zero, older);
  require(admitted(held)==prefix(Sun1) && held.header.meshletCount==9,
          ("a bin left out the frame before needs an eighth of the cap spare to come back"+demand(held)).c_str());

  // drawn the frame before, mip 1 keeps the whole cap: 14 pairs, exactly the cap
  const Frame exact = run(false, 14, prefix(Sun2), 0, zero, older);
  require(admitted(exact)==prefix(Sun2) && exact.header.meshletCount==14 && exact.header.pairSpill==0,
          ("a bin drawn the frame before goes in up to the cap and exactly at it"+demand(exact)).c_str());
  }
