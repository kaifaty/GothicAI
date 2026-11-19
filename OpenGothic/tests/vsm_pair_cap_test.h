#pragma once

// A part of material_texture_test.cpp, included inside its unnamed namespace after `require`.
// The meshlet-page pairs of the sun's virtual shadow map and the lamps' faces (reports/engine/2026-11-16-modern-trees/
// shadow-flicker.md): the engine's own vsm_clear.comp, vsm_visibility_pass.comp, vsm_cull_meshlets.comp and
// vsm_pack_draws.comp on four page entries - mip 0, mip 1, a lamp's face and mip 2 - and two clusters. An object's
// cluster keeps the meshlets whose own spheres, grown by the instance's scale and the cluster's room for the wind,
// touch an entry; a cluster of one meshlet goes whole. The bins of pairs go in whole in their order,
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

  // cluster 0: an object of seven meshlets at the instance's origin, the instance scaled by 2. Its cluster's sphere (400)
  // is 10 over its mesh's (bboxRadius 390): the room of the wind and the wave, so each meshlet's sphere grows by 10 in the
  // mesh, then by the scale: radius (2+10)*2 = 24 in the world. Its meshlets' spheres: 0, 1, 5 and 6 touch all sun
  // entries, 2 those of mips 1 and 2, 3 and 4 none, and all but 3 and 4 the lamp's face; 6 (x 84) touches mip 0's
  // page (x 0..64) only by its grown radius. Cluster 1: one meshlet of the landscape, whole in each entry but mip 2's,
  // where it is too small (the visibility pass's 32 pixels)
  const float meshlets[7][4] = {{16,16,0,2}, {20,10,0,2}, {50,16,0,2}, {-1500,16,0,2}, {16,-1500,0,2}, {30,30,0,2},
                                {42,16,0,2}};
  std::vector<float> spheres(32*4, 0.f);
  for(size_t i=0; i<spheres.size(); i+=4)
    spheres[i+3] = 1.e6f; // any sphere read past the mesh's own would take every meshlet
  for(uint32_t i=0; i<7; ++i)
    std::memcpy(&spheres[(SpheresAt+ObjectMeshlet+i)*4], meshlets[i], sizeof(meshlets[i]));
  const Cluster clusters[2] = {
    {{0.f, 0.f, 0.f, 400.f}, (0u << 16) | 0u, ObjectMeshlet, 7, 0u},
    {{32.f, 32.f, 0.f, 20.f}, (1u << 16) | 1u, 77, 1, 0xFFFFFFFFu},
    };
  std::vector<uint32_t> bucket(2*36, 0); // Bucket of scene.glsl, std140: bboxRadius at 10, meshletSpheres at 32
  const float radius = 390.f;
  std::memcpy(&bucket[10], &radius, sizeof(radius));
  bucket[32]    = SpheresAt;
  bucket[36+32] = 0xFFFFFFFFu;
  std::vector<float> instance(16, 0.f);
  instance[0] = instance[4] = instance[8] = 2.f; // mat4x3 by columns, then fatness, animPtr, deformPtr, deformCount
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
  const uint32_t noLod[2] = {};
  const uint32_t lodRecord[12] = {};
  auto lodIndices  = device.ssbo(noLod, sizeof(noLod));
  auto lodRecords  = device.ssbo(lodRecord, sizeof(lodRecord));
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

      struct Push {
        uint32_t meshletCount, candidateCap, padding[2];
        float lodPx, lodProjection, lodHeight;
        uint32_t lodDebug;
        float lodCamera[4], lodForwardAndGuard[4];
        } push = {2, cap, {}, 0.f, 0.f, 0.f, 0, {}, {}};
      static_assert(sizeof(Push)==64);
      enc.setBinding(0, sceneUbo);
      enc.setBinding(1, candidates);
      enc.setBinding(2, instanceBuf);
      enc.setBinding(3, bucketBuf);
      enc.setBinding(4, cmdBuf);
      enc.setBinding(5, clusterBuf);
      enc.setBinding(6, lightBuf);
      enc.setBinding(7, pageHiZ);
      enc.setBinding(8, pageBuf);
      enc.setBinding(9, dbg);
      enc.setBinding(10, lodIndices);
      enc.setBinding(11, lodRecords);
      enc.setPushData(&push, sizeof(push));
      enc.setPipeline(shaders.vsmVisibilityPass);
      enc.dispatchThreads(2, 17);

      struct CullPush { uint32_t pairCap, emit; } cull = {cap, 0};
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

  // the packing skips a slot the meshlets' pass reserved and left (VSM_PAIR_SKIP): draw 0 reserved two, found one. The
  // skipped slot names draw 0, which exists: without the filter draw 0 would take it, two pairs instead of one
  {
  std::vector<uint32_t> slots = older;
  const uint32_t first[3][4] = {{0, ObjectMeshlet, 0, 0}, {0, Old, 0, Skip}, {0xFFFFFFFFu, 77, (1u << 16) | 1u, 0}};
  std::memcpy(slots.data(), first, sizeof(first));
  std::vector<Cmd> counts = zero;
  counts[0].instanceCount = 2;
  counts[1].instanceCount = 1;
  const Frame f = run(true, Slots, {}, 3, counts, slots);
  require(f.header.pairSpill==0 && f.draws[0].instanceCount==1 && f.draws[1].writeOffset==2 &&
          f.draws[1].instanceCount==1 && f.packed[1]==ObjectMeshlet && f.packed[2*4+1]==77,
          "the packing skips a slot left with VSM_PAIR_SKIP, and no draw takes it");
  }

  // vsm_clear.comp at a frame's start: the demand, the candidates and the whole clusters' pairs of the frame before are
  // zeroed, and the bins it drew become the bins drawn the frame before - what the hysteresis below is given
  {
  std::vector<uint8_t> pages(shaders.vsmClear.sizeofBuffer(0, 16), 0);
  Header header = {};
  header.candidateCount = 5;
  header.clusterPairs   = 9;
  for(uint32_t i=0; i<9; ++i) {
    header.admitBits[i]     = 0x9E3779B9u*(i+1);
    header.admitPrevBits[i] = ~header.admitBits[i];
    }
  std::fill(std::begin(header.pairDemand), std::end(header.pairDemand), 7u);
  std::memcpy(pages.data(), &header, sizeof(header));
  auto pageBuf = device.ssbo(pages.data(), pages.size());
  auto command = device.commandBuffer();
  {
  auto enc = command.startEncoding(device);
  enc.setBinding(0, pageBuf);
  enc.setBinding(1, pageHiZ);
  enc.setBinding(2, scratch);
  enc.setPipeline(shaders.vsmClear);
  enc.dispatchThreads(32, 32, 16);
  }
  device.submit(command).wait();
  Header got = {};
  device.readBytes(pageBuf, &got, sizeof(got));
  require(std::all_of(std::begin(got.pairDemand), std::end(got.pairDemand), [](uint32_t d) { return d==0; }) &&
          got.candidateCount==0 && got.clusterPairs==0, "a frame starts with no demand and no candidates");
  require(std::equal(std::begin(got.admitPrevBits), std::end(got.admitPrevBits), std::begin(header.admitBits)) &&
          std::equal(std::begin(got.admitBits), std::end(got.admitBits), std::begin(header.admitBits)),
          "the bins drawn the frame before are the ones it drew");
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
  require(all.header.pairDemand[Sun0]==5 && all.header.pairDemand[Sun1]==6 && all.header.pairDemand[Lamp]==6 &&
          all.header.pairDemand[Sun2]==5,
          ("the demand of each bin: the meshlets whose spheres, grown by the scale and the room, touch its entries"+
           demand(all)).c_str());
  require(all.header.clusterPairs==4*7+3*1,
          ("whole clusters would ask a pair for each meshlet in each entry: "+std::to_string(all.header.clusterPairs)).c_str());
  require(admitted(all)==prefix(16+16*16) && all.header.meshletCount==22 && all.header.pairSpill==0,
          ("with room for all every bin is drawn"+demand(all)).c_str());
  require(pairsOf(all, 0, 0)==ids({0, 1, 5, 6}) && pairsOf(all, 0, 1)==ids({0, 1, 2, 5, 6}) &&
          pairsOf(all, 0, 2)==ids({0, 1, 2, 5, 6}) && pairsOf(all, 0, 3)==ids({0, 1, 2, 5, 6}),
          "an object's entry takes the meshlets whose spheres touch it");
  require(pairsOf(all, 1, 0).size()==1 && pairsOf(all, 1, 1).size()==1 && pairsOf(all, 1, 2).size()==1 &&
          pairsOf(all, 1, 3).empty(), "a cluster without spheres goes whole into each entry");

  // a cap of 16: the lamp's 6 and mip 0's 5 go in, mip 1's 6 would make 17: it and all after it are left out, mip 2's 5
  // too, though they would fit
  const Frame edge = run(false, 16, admitted(all), 0, zero, older);
  require(admitted(edge)==prefix(Sun1) && edge.header.meshletCount==11 && edge.header.pairSpill==0,
          ("bins go in whole in their order, lamps first; the first past the cap and all after it are left out"+
           demand(edge)).c_str());
  require(pairsOf(edge, 0, 0)==ids({0, 1, 5, 6}) && pairsOf(edge, 0, 1).empty() &&
          pairsOf(edge, 0, 2)==ids({0, 1, 2, 5, 6}) &&
          pairsOf(edge, 0, 3).empty() && pairsOf(edge, 1, 1).empty(),
          "a bin left out has no pair, the bins drawn all theirs");

  // a cap of 17 holds the lamp, mip 0 and mip 1 (17), but mip 1 was left out the frame before and needs 17 under 7/8
  // of the cap: it stays out
  const Frame held = run(false, 17, admitted(edge), 0, zero, older);
  require(admitted(held)==prefix(Sun1) && held.header.meshletCount==11,
          ("a bin left out the frame before needs an eighth of the cap spare to come back"+demand(held)).c_str());

  // drawn the frame before, mip 1 keeps the whole cap: 17 pairs, exactly the cap
  const Frame exact = run(false, 17, prefix(Sun2), 0, zero, older);
  require(admitted(exact)==prefix(Sun2) && exact.header.meshletCount==17 && exact.header.pairSpill==0,
          ("a bin drawn the frame before goes in up to the cap and exactly at it"+demand(exact)).c_str());
  }

// The layout of the sun's entries and the selection over frames (shadow-flicker.md, «Третий круг»): the engine's own
// vsm_clear, vsm_mark_pages, vsm_clump_pages, vsm_list_pages, vsm_alloc_pages, vsm_merge_pages, vsm_visibility_pass and
// vsm_cull_meshlets on a screen of 32x32 pixels, a pixel marking a cell of mip 0's page table. An entry takes the bin of
// its farthest page (vsmSunBin). Before, quads took their grid from the first marked page of a stripe of rows and
// stripes ran across rings, so a page's bin followed the marks of TAA's jittered frame; the layout split by the cut
// made the demand follow the cut, and the cut swung from frame to frame. Now the layout is the marks' on the grid of
// the table: the same whatever bins the frame before drew, one more mark changes no entry but its own, each entry lies
// in one ring, and with the demand at the edge of the hysteresis the same bins are drawn frame after frame.
void vsmLayoutRecords(Tempest::Device& device) {
  Resources resources(device);
  Shaders   shaders;
  shaders.waitCompiler();
  if(!Shaders::isVsmSupported()) {
    std::cout << "vsm layout: skipped, no virtual shadow maps on this device\n";
    return;
    }

  constexpr int      Tbl  = 32;      // VSM_PAGE_TBL_SIZE
  constexpr uint32_t Pool = 64*64;   // the pool's pages, as the game's 8192x8192
  constexpr uint32_t Sun0 = 16;      // the bin of mip 0's centre ring: the lamps' 16 bins come first
  constexpr uint32_t Meshlets = 10;  // of the one cluster, whole in every entry
  struct Cluster { float sphere[4]; uint32_t bucketId_commandId, firstMeshlet; int32_t meshletCount; uint32_t instanceId; };
  struct Light   { float pos[3], range, color[3], cookie, sourceSize, nearLimit, padding[2]; }; // LightSource, scene.glsl
  struct Entry   { int x, y, mip, w, h; };
  const size_t headerWords = shaders.vsmClear.sizeofBuffer(0, 0)/sizeof(uint32_t);
  const size_t admitAt     = 4 + 16 + 16*4 + 8; // VsmHeader: counts, pagePerMip, pageBbox, then 8 counters
  const size_t demandAt    = admitAt + 2*9;     // after admitBits and admitPrevBits
  require(sizeof(Cluster)==shaders.vsmCullMeshlets.sizeofBuffer(5, 1), "the test's Cluster follows scene.glsl");
  const auto ring = [](int x, int y) { // vsmSunBin's ring of a page: the square around the table's centre, 2 pages wide
    return std::max(std::max(x-Tbl/2, Tbl/2-1-x), std::max(y-Tbl/2, Tbl/2-1-y))/2;
    };
  const auto prefix = [](uint32_t bins) {
    std::array<uint32_t,9> ret = {};
    for(uint32_t b=0; b<bins; ++b)
      ret[b/32] |= 1u << (b%32);
    return ret;
    };

  // the screen is the page table: the camera's and the sun's views are the same, lifted 100 above the planet's
  // surface so the planet does not hide the sun; a pixel's derivatives are a texel of mip 0. The sun's view of the
  // clusters scales x and y by 1/1024 per cm, as the game's (vsm_cull.glsl)
  SceneGlobals::UboGlobal ubo;
  ubo.viewProject = Tempest::Matrix4x4::mkIdentity();
  ubo.viewProject.scale(1.f/1024.f, 1.f/1024.f, 1.f);
  ubo.viewProjectLwcInv = Tempest::Matrix4x4::mkIdentity();
  ubo.viewProjectLwcInv.translate(0, 100, 0);
  ubo.viewVirtualShadowLwc = Tempest::Matrix4x4::mkIdentity();
  ubo.viewVirtualShadowLwc.translate(0, -100, 0);
  ubo.vsmDdx       = Tempest::Vec4(1.f/4096.f, 0, 0, 0);
  ubo.vsmDdy       = Tempest::Vec4(0, 1.f/4096.f, 0, 0);
  ubo.sunDir       = Tempest::Vec3(0, 0.6f, 0.8f); // the normal of a zero G-buffer texel is (0,0,1)
  ubo.screenResInv = Tempest::Vec2(1.f/Tbl, 1.f/Tbl);
  auto sceneUbo = device.ssbo(&ubo, sizeof(ubo));

  Tempest::Pixmap diffusePm(Tbl, Tbl, Tempest::TextureFormat::RGBA8);
  Tempest::Pixmap normalPm (Tbl, Tbl, Tempest::TextureFormat::R32U);
  std::memset(diffusePm.data(), 0, diffusePm.dataSize());
  std::memset(normalPm.data(),  0, normalPm.dataSize());
  auto diffuse = device.texture(diffusePm, false);
  auto normal  = device.texture(normalPm,  false);
  auto pageTbl = device.image3d(Tempest::TextureFormat::R32U, Tbl, Tbl, 16);
  auto pageHiZ = device.image3d(Tempest::TextureFormat::R32U, Tbl, Tbl, 16);
  const std::vector<uint32_t> noFaces(6, 0u); // a lamp's six faces, none marked
  auto omni    = device.ssbo(noFaces.data(), noFaces.size()*sizeof(uint32_t));
  auto pageTmp = device.ssbo(Tempest::Uninitialized, shaders.vsmAllocPages.sizeofBuffer(3, Pool));
  auto dbg     = device.image2d(Tempest::TextureFormat::R32U, 64, 64);

  // one cluster of the landscape kind (no spheres of its meshlets: whole in each entry), over all of mip 0 and in
  // front of the marked receivers (depth 0.5)
  const Cluster cluster = {{0.f, 0.f, 0.6f, 1024.f}, 0u, 0, int32_t(Meshlets), 0xFFFFFFFFu};
  std::vector<uint32_t> bucket(36, 0);     // Bucket of scene.glsl, std140: meshletSpheres at 32
  bucket[32] = 0xFFFFFFFFu;
  const Light light = {{0.f, 0.f, 0.f}, 1.f, {1.f, 1.f, 1.f}, 0.f, 0.f, 0.f, {0.f, 0.f}};
  std::vector<float> instance(16, 0.f), spheres(4, 0.f);
  auto clusterBuf  = device.ssbo(&cluster, sizeof(cluster));
  auto bucketBuf   = device.ssbo(bucket.data(), bucket.size()*sizeof(uint32_t));
  auto lightBuf    = device.ssbo(&light, sizeof(light));
  auto instanceBuf = device.ssbo(instance.data(), instance.size()*sizeof(float));
  auto sphereBuf   = device.ssbo(spheres.data(), spheres.size()*sizeof(float));
  const uint32_t noLod[1] = {};        // the cluster has no levels of the cluster hierarchy (cluster_lod.glsl)
  const uint32_t lodRecord[12] = {};
  auto lodIndices  = device.ssbo(noLod, sizeof(noLod));
  auto lodRecords  = device.ssbo(lodRecord, sizeof(lodRecord));

  // a frame of renderer_vsm.cpp from the marks to the entries and, with a cap, the meshlets' pass under it. `words` is
  // the buffer of the header and the entries: its admitBits are the bins the frame before drew (vsm_clear.comp moves
  // them), and the frame leaves its own there
  const auto frame = [&](const std::vector<std::pair<int,int>>& marks, std::vector<uint32_t>& words, uint32_t cap) {
    Tempest::Pixmap depthPm(Tbl, Tbl, Tempest::TextureFormat::R32F);
    auto* z = reinterpret_cast<float*>(depthPm.data());
    std::fill(z, z+Tbl*Tbl, 1.f); // the sky marks nothing
    for(auto [x, y] : marks)
      z[y*Tbl + x] = 0.5f;
    auto depth      = device.texture(depthPm, false);
    auto pages      = device.ssbo(words.data(), words.size()*sizeof(uint32_t));
    auto candidates = device.ssbo(Tempest::Uninitialized, size_t(std::max(cap, 1u))*4*sizeof(uint32_t));
    auto payload    = device.ssbo(Tempest::Uninitialized, size_t(std::max(cap, 1u))*4*sizeof(uint32_t));
    const uint32_t zero[5] = {};
    auto cmdBuf     = device.ssbo(zero, sizeof(zero));

    auto command = device.commandBuffer();
    {
    auto enc = command.startEncoding(device);
    enc.setBinding(0, pages);
    enc.setBinding(1, pageTbl);
    enc.setBinding(2, pageHiZ);
    enc.setPipeline(shaders.vsmClear);
    enc.dispatchThreads(Tbl, Tbl, 16);

    const float mipBias = 0;
    enc.setBinding(0, sceneUbo);
    enc.setBinding(1, diffuse, Tempest::Sampler::nearest());
    enc.setBinding(2, normal,  Tempest::Sampler::nearest());
    enc.setBinding(3, depth,   Tempest::Sampler::nearest());
    enc.setBinding(4, pageTbl);
    enc.setBinding(5, pageHiZ);
    enc.setPushData(&mipBias, sizeof(mipBias));
    enc.setPipeline(shaders.vsmMarkPages);
    enc.dispatchThreads(Tbl, Tbl);

    enc.setBinding(0, pages);
    enc.setBinding(1, pageTbl);
    enc.setPipeline(shaders.vsmClumpPages);
    enc.dispatchThreads(Tbl, Tbl, 16);

    enc.setBinding(0, pages);
    enc.setBinding(1, pageTbl);
    enc.setBinding(2, omni);
    enc.setBinding(3, pageTmp);
    enc.setBinding(4, dbg);
    enc.setPipeline(shaders.vsmListPages);
    enc.dispatch(16+1); // the last group lists the lamps' faces and counts the entries
    enc.setPipeline(shaders.vsmAllocPages);
    enc.dispatch(1);
    enc.setPipeline(shaders.vsmMergePages);
    enc.dispatch(1);

    if(cap>0) {
      struct Push {
        uint32_t meshletCount, candidateCap, padding[2];
        float lodPx, lodProjection, lodHeight;
        uint32_t lodDebug;
        float lodCamera[4], lodForwardAndGuard[4];
        } push = {1, cap, {}, 0.f, 0.f, 0.f, 0, {}, {}};
      static_assert(sizeof(Push)==64);
      enc.setBinding(0, sceneUbo);
      enc.setBinding(1, candidates);
      enc.setBinding(2, instanceBuf);
      enc.setBinding(3, bucketBuf);
      enc.setBinding(4, cmdBuf);
      enc.setBinding(5, clusterBuf);
      enc.setBinding(6, lightBuf);
      enc.setBinding(7, pageHiZ);
      enc.setBinding(8, pages);
      enc.setBinding(9, dbg);
      enc.setBinding(10, lodIndices);
      enc.setBinding(11, lodRecords);
      enc.setPushData(&push, sizeof(push));
      enc.setPipeline(shaders.vsmVisibilityPass);
      enc.dispatchThreads(1, 17);

      struct CullPush { uint32_t pairCap, emit; } cull = {cap, 0};
      enc.setBinding(7, sphereBuf);
      enc.setBinding(9, payload);
      enc.setPipeline(shaders.vsmCullMeshlets);
      for(cull.emit=0; cull.emit<2; ++cull.emit) {
        enc.setPushData(&cull, sizeof(cull));
        enc.dispatch(3);
        }
      }
    }
    device.submit(command).wait();
    device.readBytes(pages, words.data(), words.size()*sizeof(uint32_t));

    std::vector<Entry> ret;
    for(uint32_t i=0; i<words[0] && headerWords+i<words.size(); ++i) {
      const uint32_t p = words[headerWords+i];
      if(p==0 || (p & 1u)!=0 || ((p >> 24) & 0xF)==0)
        continue; // none, a lamp's face, or a page of a block placed with its first one
      ret.push_back(Entry{int((p >> 1) & 0xFF), int((p >> 9) & 0xFF), int((p >> 17) & 0x7F),
                          int((p >> 24) & 0xF), int((p >> 28) & 0xF)});
      }
    return ret;
    };
  const auto header = [&](const std::array<uint32_t,9>& drawnBefore) {
    std::vector<uint32_t> words(std::max(shaders.vsmClear.sizeofBuffer(0, Pool)/sizeof(uint32_t), demandAt+272), 0u);
    for(size_t i=0; i<drawnBefore.size(); ++i)
      words[admitAt+i] = drawnBefore[i];
    return words;
    };
  const auto admitted = [&](const std::vector<uint32_t>& words) {
    std::array<uint32_t,9> ret = {};
    for(size_t i=0; i<ret.size(); ++i)
      ret[i] = words[admitAt+i];
    return ret;
    };
  const auto text = [](const std::vector<Entry>& es) {
    std::string ret;
    for(auto& e : es)
      ret += " (" + std::to_string(e.x) + "," + std::to_string(e.y) + " " + std::to_string(e.w) + "x" + std::to_string(e.h) + ")";
    return ret;
    };
  const auto has = [](const std::vector<Entry>& es, Entry e) {
    return std::any_of(es.begin(), es.end(), [&](const Entry& a) {
      return a.x==e.x && a.y==e.y && a.mip==e.mip && a.w==e.w && a.h==e.h;
      });
    };
  const auto layout = [&](const std::vector<std::pair<int,int>>& marks, const std::array<uint32_t,9>& drawnBefore) {
    auto words = header(drawnBefore);
    return frame(marks, words, 0);
    };

  // a row through the centre, x 10..21 of y 16 (rings 2, 1, 0, 0, 1, 2), and a block of 4x4 pages at x 17..20,
  // y 12..15, half a quad of the grid off it on the left
  std::vector<std::pair<int,int>> marks;
  for(int x=10; x<=21; ++x)
    marks.push_back({x, 16});
  for(int x=17; x<=20; ++x)
    for(int y=12; y<=15; ++y)
      marks.push_back({x, y});

  const auto base = layout(marks, {});
  for(auto& e : base) {
    bool one = e.mip==0 && (e.h==1 || (e.x%2==0 && e.y%2==0));
    for(int x=e.x; x<e.x+e.w; ++x)
      for(int y=e.y; y<e.y+e.h; ++y)
        one &= ring(x, y)==ring(e.x, e.y);
    require(one && e.x/8==(e.x+e.w-1)/8,
            ("an entry lies in one ring, its quads on the table's grid, its stripe in one of the grid's columns:"+text(base)).c_str());
    }
  for(auto [x, y] : marks) {
    const auto n = std::count_if(base.begin(), base.end(), [x=x, y=y](const Entry& e) {
      return e.x<=x && x<e.x+e.w && e.y<=y && y<e.y+e.h;
      });
    require(n==1, ("each marked page is in one entry:"+text(base)).c_str());
    }
  require(base.size()==11 && has(base, {16, 12, 0, 4, 2}) && has(base, {20, 12, 0, 2, 2}) &&
          has(base, {14, 16, 0, 2, 1}) && has(base, {16, 16, 0, 2, 1}) && has(base, {10, 16, 0, 2, 1}),
          ("quads of the grid, stripes cut at each ring and at column 16:"+text(base)).c_str());

  // the same layout whatever bins the frame before drew
  for(auto drawnBefore : {prefix(16+16*16), prefix(Sun0+1), prefix(Sun0+2), prefix(Sun0+16+1)}) {
    const auto es = layout(marks, drawnBefore);
    bool same = es.size()==base.size();
    for(auto& e : es)
      same &= has(base, e);
    require(same, ("the layout does not follow the bins the frame before drew:"+text(base)+" |"+text(es)).c_str());
    }

  // one more mark far off and one at the row's end, as the jitter marks a page at an edge or not: no entry of the
  // other pages changes, the new ones are entries of their own
  {
  auto more = marks;
  more.push_back({3, 3});
  more.push_back({22, 16});
  const auto es = layout(more, {});
  bool kept = es.size()==base.size()+2 && has(es, {3, 3, 0, 1, 1}) && has(es, {22, 16, 0, 1, 1});
  for(auto& e : base)
    kept &= has(es, e);
  require(kept, ("one more mark changes no entry but its own:"+text(base)+" |"+text(es)).c_str());
  }

  // the selection frame after frame with the demand at the edge: the cluster asks 10 pairs of each entry, 30 of ring 0,
  // 40 of ring 1 and 40 of ring 2. Under a cap of 72 ring 1 goes in with the whole cap (70) but not under 7/8 of it
  // (63): from a frame that drew none of mip 0 ring 1 stays out, from one that drew all it stays in - and the layout,
  // the demand and the bins drawn are the same in every frame
  struct Run { std::array<uint32_t,9> start, drawn; uint32_t pairs; };
  for(const Run& run : {Run{prefix(0), prefix(Sun0+1), 30u}, Run{prefix(16+16*16), prefix(Sun0+2), 70u}}) {
    auto words = header(run.start);
    for(int f=0; f<4; ++f) {
      const auto es = frame(marks, words, 72);
      const uint32_t* d = &words[demandAt];
      require(es.size()==base.size() && d[Sun0]==30 && d[Sun0+1]==40 && d[Sun0+2]==40,
              ("the demand of each ring is the same in every frame: "+std::to_string(d[Sun0])+", "+
               std::to_string(d[Sun0+1])+", "+std::to_string(d[Sun0+2])+","+text(es)).c_str());
      require(admitted(words)==run.drawn && words[1]==run.pairs,
              ("with the demand at the edge the same bins are drawn frame after frame: frame "+std::to_string(f)+
               ", drawn "+std::to_string(words[1])).c_str());
      }
    }
  }

// the records of this header, one call of material_texture_test.cpp
void vsmRecords(Tempest::Device& device) {
  vsmPairCapRecords(device);
  vsmLayoutRecords(device);
  }
